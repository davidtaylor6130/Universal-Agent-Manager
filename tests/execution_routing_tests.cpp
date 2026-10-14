#include "test_harness.h"
#include "app/execution_routing_service.h"
#include "common/config/provider_availability.h"

#include <barrier>

using namespace uam_test;

namespace
{
	uam::ExecutionRoutingCandidate RoutingHost(const std::string& id, std::size_t slots = 1)
	{
		uam::ExecutionRoutingCandidate candidate;
		candidate.host_id = id;
		candidate.connection_identity = "connection-" + id;
		candidate.confirmed_connection_identity = candidate.connection_identity;
		candidate.provider_configuration_identity = "provider-config";
		candidate.confirmed_provider_configuration_identity = candidate.provider_configuration_identity;
		candidate.pool_enabled = true;
		candidate.provider_confirmed = true;
		candidate.runner_ready = true;
		candidate.runner_compatible = true;
		candidate.provider_supported = true;
		candidate.max_active_turns = slots;
		candidate.mappings.push_back({"local:/repo", "folder-" + id, "/mapped/repo"});
		return candidate;
	}

	uam::ExecutionRoutingRequest RoutingRequest(const std::string& chat_id)
	{
		uam::ExecutionRoutingRequest request;
		request.chat_id = chat_id;
		request.automatic_enabled = true;
		request.source_workspace_identity = "local:/repo";
		return request;
	}
}

UAM_TEST(ExecutionRoutingUsesLeastOccupiedEligibleHostAndStableTieBreak)
{
	uam::ExecutionRoutingService service;
	const std::vector<uam::ExecutionRoutingCandidate> hosts{RoutingHost("z", 3), RoutingHost("a", 3)};
	const uam::ExecutionRoutingDecision tie = service.Admit(RoutingRequest("first"), hosts, {});
	UAM_ASSERT_EQ(tie.status, uam::ExecutionRoutingStatus::Assigned);
	UAM_ASSERT_EQ(tie.assignment.host_id, std::string("a"));
	const uam::ExecutionRoutingDecision next = service.Admit(RoutingRequest("second"), hosts, {{"first", "a"}, {"first", "a"}});
	UAM_ASSERT_EQ(next.assignment.host_id, std::string("z"));
	UAM_ASSERT_EQ(next.assignment.destination_folder_id, std::string("folder-z"));
}

UAM_TEST(ExecutionRoutingRequiresConfirmedCurrentCapabilityAndExactWorkspace)
{
	const uam::ExecutionRoutingCandidate ready = RoutingHost("remote");
	for (int invalidation = 0; invalidation < 8; ++invalidation)
	{
		uam::ExecutionRoutingService service;
		uam::ExecutionRoutingCandidate candidate = ready;
		switch (invalidation)
		{
		case 0: candidate.pool_enabled = false; break;
		case 1: candidate.provider_confirmed = false; break;
		case 2: candidate.runner_ready = false; break;
		case 3: candidate.runner_compatible = false; break;
		case 4: candidate.provider_supported = false; break;
		case 5: candidate.confirmed_connection_identity = "stale"; break;
		case 6: candidate.confirmed_provider_configuration_identity = "stale"; break;
		case 7: candidate.max_active_turns = 0; break;
		}
		UAM_ASSERT_EQ(service.Admit(RoutingRequest("candidate"), {candidate}, {}).status, uam::ExecutionRoutingStatus::Waiting);
	}
	uam::ExecutionRoutingService service;
	uam::ExecutionRoutingRequest request = RoutingRequest("worktree");
	request.source_workspace_identity = "local:/repo/worktrees/branch";
	UAM_ASSERT_EQ(service.Admit(request, {ready}, {}).status, uam::ExecutionRoutingStatus::Waiting);
	request.automatic_enabled = false;
	UAM_ASSERT_EQ(service.Admit(request, {ready}, {}).status, uam::ExecutionRoutingStatus::Disabled);
	UAM_ASSERT_EQ(service.Admit(RoutingRequest("no-local-fallback"), {RoutingHost("local")}, {}).status, uam::ExecutionRoutingStatus::Waiting);
}

UAM_TEST(ExecutionRoutingConcurrentAdmissionsReserveOneSlotAtomically)
{
	uam::ExecutionRoutingService service;
	const std::vector<uam::ExecutionRoutingCandidate> hosts{RoutingHost("remote")};
	std::barrier gate(3);
	uam::ExecutionRoutingDecision first;
	uam::ExecutionRoutingDecision second;
	std::jthread first_thread([&] { gate.arrive_and_wait(); first = service.Admit(RoutingRequest("first"), hosts, {}); });
	std::jthread second_thread([&] { gate.arrive_and_wait(); second = service.Admit(RoutingRequest("second"), hosts, {}); });
	gate.arrive_and_wait();
	first_thread.join();
	second_thread.join();
	const int admitted = static_cast<int>(first.status == uam::ExecutionRoutingStatus::Assigned) +
	    static_cast<int>(second.status == uam::ExecutionRoutingStatus::Assigned);
	UAM_ASSERT_EQ(admitted, 1);
}

UAM_TEST(ExecutionRoutingConcurrentTwoHostOwnersStayPinnedThroughRestartAndRelease)
{
	uam::ExecutionRoutingService service;
	const std::vector<uam::ExecutionRoutingCandidate> hosts{RoutingHost("linux"), RoutingHost("windows")};
	std::barrier gate(4);
	std::vector<uam::ExecutionRoutingDecision> decisions(3);
	std::vector<std::jthread> threads;
	for (std::size_t index = 0; index < decisions.size(); ++index)
	{
		threads.emplace_back([&, index]
		{
			gate.arrive_and_wait();
			decisions[index] = service.Admit(RoutingRequest("chat-" + std::to_string(index)), hosts, {});
		});
	}
	gate.arrive_and_wait();
	for (std::jthread& thread : threads) thread.join();
	std::vector<uam::ExecutionRoutingAssignment> owners;
	std::string waiting_id;
	for (std::size_t index = 0; index < decisions.size(); ++index)
	{
		if (decisions[index].status == uam::ExecutionRoutingStatus::Assigned)
			owners.push_back(decisions[index].assignment);
		else
		{
			UAM_ASSERT_EQ(decisions[index].status, uam::ExecutionRoutingStatus::Waiting);
			UAM_ASSERT(decisions[index].assignment.host_id.empty());
			waiting_id = "chat-" + std::to_string(index);
		}
	}
	UAM_ASSERT_EQ(owners.size(), std::size_t(2));
	UAM_ASSERT(owners[0].host_id != owners[1].host_id);
	UAM_ASSERT(!waiting_id.empty());
	uam::ExecutionRoutingService restored;
	UAM_ASSERT(restored.RestoreReservations(owners));
	UAM_ASSERT_EQ(restored.Admit(RoutingRequest(waiting_id), hosts, {}).status, uam::ExecutionRoutingStatus::Waiting);
	UAM_ASSERT(!restored.Release(owners[0].chat_id, false));
	UAM_ASSERT(restored.Release(owners[1].chat_id, true));
	std::vector<uam::ExecutionRoutingCandidate> unavailable = hosts;
	for (uam::ExecutionRoutingCandidate& host : unavailable)
	{
		if (host.host_id == owners[0].host_id) host.runner_ready = false;
	}
	const uam::ExecutionRoutingDecision pinned = restored.Admit(RoutingRequest(owners[0].chat_id), unavailable, {});
	UAM_ASSERT_EQ(pinned.reason, uam::ExecutionRoutingReason::HostUnavailable);
	UAM_ASSERT_EQ(pinned.assignment, owners[0]);
	const uam::ExecutionRoutingDecision released = restored.Admit(RoutingRequest(waiting_id), unavailable, {});
	UAM_ASSERT_EQ(released.status, uam::ExecutionRoutingStatus::Assigned);
	UAM_ASSERT_EQ(released.assignment.host_id, owners[1].host_id);
	UAM_ASSERT_EQ(restored.Admit(RoutingRequest(owners[0].chat_id), hosts, {}).assignment, owners[0]);
	UAM_ASSERT_EQ(restored.Admit(RoutingRequest("still-full"), hosts, {}).status, uam::ExecutionRoutingStatus::Waiting);
}

UAM_TEST(ExecutionRoutingExplicitOverrideBypassesAutoSlotsButConsumesOccupancy)
{
	uam::ExecutionRoutingService service;
	uam::ExecutionRoutingCandidate host = RoutingHost("remote");
	host.provider_confirmed = false;
	uam::ExecutionRoutingRequest explicit_request = RoutingRequest("explicit");
	explicit_request.automatic_enabled = false;
	explicit_request.explicit_host_id = "remote";
	explicit_request.explicit_folder_id = "original-folder";
	explicit_request.explicit_directory = "/original/cwd";
	const uam::ExecutionRoutingDecision explicit_result = service.Admit(explicit_request, {host}, {{"busy-chat", "remote"}});
	UAM_ASSERT_EQ(explicit_result.status, uam::ExecutionRoutingStatus::Assigned);
	UAM_ASSERT_EQ(explicit_result.assignment.destination_directory, std::string("/original/cwd"));
	host.provider_confirmed = true;
	host.max_active_turns = 2;
	UAM_ASSERT_EQ(service.Admit(RoutingRequest("automatic"), {host}, {{"busy-chat", "remote"}}).status, uam::ExecutionRoutingStatus::Waiting);
	UAM_ASSERT(!service.Release("explicit", false));
	UAM_ASSERT(service.Release("explicit", true));
	UAM_ASSERT_EQ(service.Admit(RoutingRequest("automatic"), {host}, {{"busy-chat", "remote"}}).status, uam::ExecutionRoutingStatus::Assigned);
}

UAM_TEST(ExecutionRoutingRestartPreservesUncertainOwnerAndOriginalDestination)
{
	uam::ExecutionRoutingService original;
	const uam::ExecutionRoutingDecision assigned = original.Admit(RoutingRequest("accepted"), {RoutingHost("a")}, {});
	uam::ExecutionRoutingService restored;
	UAM_ASSERT(restored.RestoreReservations({assigned.assignment}));
	UAM_ASSERT(!restored.Release("accepted", false));
	uam::ExecutionRoutingCandidate changed_mapping = RoutingHost("a");
	changed_mapping.mappings.front().destination_directory = "/different/repo";
	const uam::ExecutionRoutingDecision pinned = restored.Admit(RoutingRequest("accepted"), {changed_mapping, RoutingHost("b")}, {{"accepted", "a"}});
	UAM_ASSERT_EQ(pinned.status, uam::ExecutionRoutingStatus::Assigned);
	UAM_ASSERT_EQ(pinned.assignment.destination_directory, std::string("/mapped/repo"));
	changed_mapping.runner_ready = false;
	UAM_ASSERT_EQ(restored.Admit(RoutingRequest("accepted"), {changed_mapping}, {}).reason, uam::ExecutionRoutingReason::HostUnavailable);
	changed_mapping.runner_ready = true;
	changed_mapping.provider_configuration_identity = "different-provider-config";
	UAM_ASSERT_EQ(restored.Admit(RoutingRequest("accepted"), {changed_mapping}, {}).reason, uam::ExecutionRoutingReason::OwnershipChanged);
	changed_mapping.provider_configuration_identity = "provider-config";
	changed_mapping.connection_identity = "different-machine";
	const uam::ExecutionRoutingDecision changed = restored.Admit(RoutingRequest("accepted"), {changed_mapping, RoutingHost("b")}, {});
	UAM_ASSERT_EQ(changed.status, uam::ExecutionRoutingStatus::Waiting);
	UAM_ASSERT_EQ(changed.reason, uam::ExecutionRoutingReason::OwnershipChanged);
	UAM_ASSERT_EQ(changed.assignment.host_id, std::string("a"));
	const uam::ExecutionRoutingDecision missing = restored.Admit(RoutingRequest("accepted"), {RoutingHost("b")}, {});
	UAM_ASSERT_EQ(missing.status, uam::ExecutionRoutingStatus::Waiting);
	UAM_ASSERT_EQ(missing.assignment.host_id, std::string("a"));
	UAM_ASSERT_EQ(restored.Admit(RoutingRequest("next"), {RoutingHost("a")}, {}).status, uam::ExecutionRoutingStatus::Waiting);
}

UAM_TEST(ExecutionRoutingRejectsConflictingSnapshotsWithoutChangingReservations)
{
	uam::ExecutionRoutingService service;
	const uam::ExecutionRoutingCandidate host = RoutingHost("a", 2);
	UAM_ASSERT_EQ(service.Admit(RoutingRequest("bad-host"), {host, host}, {}).status, uam::ExecutionRoutingStatus::Invalid);
	UAM_ASSERT_EQ(service.Admit(RoutingRequest("bad-owner"), {host}, {{"unknown-outbox", "a"}, {"unknown-outbox", "b"}}).status, uam::ExecutionRoutingStatus::Invalid);
	UAM_ASSERT_EQ(service.Admit(RoutingRequest("unknown-outbox"), {host}, {{"unknown-outbox", "a"}}).status, uam::ExecutionRoutingStatus::Invalid);
	const uam::ExecutionRoutingDecision assigned = service.Admit(RoutingRequest("accepted"), {host}, {});
	uam::ExecutionRoutingAssignment wrong = assigned.assignment;
	wrong.host_id = "b";
	UAM_ASSERT(!service.RestoreReservations({wrong}));
	UAM_ASSERT_EQ(service.Admit(RoutingRequest("accepted"), {host}, {}).assignment, assigned.assignment);
	const uam::ExecutionRoutingDecision next = service.Admit(RoutingRequest("next"), {host}, {{"accepted", "a"}, {"accepted", "a"}});
	UAM_ASSERT_EQ(next.status, uam::ExecutionRoutingStatus::Assigned);
	UAM_ASSERT_EQ(service.Admit(RoutingRequest("full"), {host}, {}).status, uam::ExecutionRoutingStatus::Waiting);
}

UAM_TEST(ExecutionRoutingAssignedIdleAutomaticChatWaitsForItsOwnHostCapacity)
{
	uam::ExecutionRoutingService service;
	const uam::ExecutionRoutingDecision initial = service.Admit(RoutingRequest("idle"), {RoutingHost("a")}, {});
	UAM_ASSERT(initial.assignment.automatic);
	UAM_ASSERT(service.Release("idle", true));
	uam::ExecutionRoutingRequest next_turn = RoutingRequest("idle");
	next_turn.assigned = initial.assignment;
	const uam::ExecutionRoutingDecision full = service.Admit(next_turn, {RoutingHost("a"), RoutingHost("b")}, {{"another-chat", "a"}});
	UAM_ASSERT_EQ(full.status, uam::ExecutionRoutingStatus::Waiting);
	UAM_ASSERT_EQ(full.assignment.host_id, std::string("a"));
	const uam::ExecutionRoutingDecision unrelated = service.Admit(RoutingRequest("unrelated"), {RoutingHost("a")}, {});
	UAM_ASSERT_EQ(unrelated.status, uam::ExecutionRoutingStatus::Assigned);
	UAM_ASSERT(service.Release("unrelated", true));
	const uam::ExecutionRoutingDecision available = service.Admit(next_turn, {RoutingHost("a")}, {});
	UAM_ASSERT_EQ(available.status, uam::ExecutionRoutingStatus::Assigned);
	UAM_ASSERT_EQ(available.assignment, initial.assignment);
}

UAM_TEST(ExecutionRoutingOnlyOwnedRecoveryBypassesChangedOptInAndFullCapacity)
{
	uam::ExecutionRoutingService original;
	const uam::ExecutionRoutingDecision initial = original.Admit(RoutingRequest("owned"), {RoutingHost("a")}, {});
	uam::ExecutionRoutingService restored;
	UAM_ASSERT(restored.RestoreReservations({initial.assignment}));
	uam::ExecutionRoutingCandidate unconfirmed = RoutingHost("a");
	unconfirmed.provider_confirmed = false;
	unconfirmed.pool_enabled = false;
	uam::ExecutionRoutingRequest request = RoutingRequest("owned");
	request.assigned = initial.assignment;
	request.automatic_enabled = false;
	UAM_ASSERT_EQ(restored.Admit(request, {unconfirmed, RoutingHost("b")}, {{"another-chat", "a"}}).status, uam::ExecutionRoutingStatus::Waiting);
	request.recovery_of_owned_work = true;
	const uam::ExecutionRoutingDecision recovered = restored.Admit(request, {unconfirmed, RoutingHost("b")}, {{"another-chat", "a"}});
	UAM_ASSERT_EQ(recovered.status, uam::ExecutionRoutingStatus::Assigned);
	UAM_ASSERT_EQ(recovered.assignment, initial.assignment);
	unconfirmed.runner_ready = false;
	UAM_ASSERT_EQ(restored.Admit(request, {unconfirmed, RoutingHost("b")}, {}).reason, uam::ExecutionRoutingReason::HostUnavailable);
	request.chat_id = "unknown";
	request.assigned = {};
	UAM_ASSERT_EQ(restored.Admit(request, {RoutingHost("b")}, {}).status, uam::ExecutionRoutingStatus::Invalid);
}

#include "app/execution_routing_integration.h"
#include "remote/runner_protocol.h"
#include "common/runtime/provider_cli_compatibility_service.h"

namespace
{
	void ConfigureRoutingFixture(uam::AppState& app, const fs::path& root)
	{
		app.data_root = root / "data";
		app.provider_profiles.push_back(ProviderProfileStore::DefaultCodexProfile());
		ExecutionHost host;
		host.id = "remote";
		host.transport = "ssh";
		host.ssh_alias = "fixture-no-ssh";
		host.platform = "linux";
		host.architecture = "x86_64";
		host.runner_status = "ready";
		host.runner_version = "test-version";
		host.runner_protocol_version = uam::remote::kRunnerProtocolVersion;
		app.settings.execution_hosts.push_back(host);
		uam::CliProviderVersionState version;
		version.provider_id = "codex-cli";
		version.execution_host = host;
		version.checked = version.supported = true;
		version.installed_version = "0.114.0";
		app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey("codex-cli", host.id)] = version;
		app.folders.push_back({"source", "Source", (root / "repo").string(), false, "local"});
		app.folders.push_back({"destination", "Destination", "/mapped/repo", false, "remote"});
		app.settings.execution_routing.enabled = true;
		ExecutionRoutingHostPolicy policy;
		policy.host_id = "remote";
		policy.enabled = true;
		policy.max_active_turns = 1;
		policy.provider_confirmations.push_back({"codex-cli", uam::ExecutionRoutingConnectionIdentity(host),
		    uam::ExecutionRoutingProviderIdentity(app, app.provider_profiles.front())});
		policy.workspace_mappings.push_back({"source", (root / "repo").string(), "destination"});
		app.settings.execution_routing.hosts.push_back(policy);
		ChatSession chat = ChatDomainService().CreateNewChat("source", "codex-cli");
		chat.id = "automatic-chat";
		chat.workspace_directory = (root / "repo").string();
		chat.routing_mode = "automatic";
		chat.routing_source_folder_id = "source";
		chat.routing_source_directory = chat.workspace_directory;
		app.chats.push_back(chat);
	}
}

UAM_TEST(ExecutionRoutingSettingsRoundTripAndInvalidInputAreAtomic)
{
	TempDir temp("uam-routing-settings");
	uam::AppState app;
	ConfigureRoutingFixture(app, temp.root);
	const nlohmann::json original = SettingsStore::SerializeExecutionRoutingSettings(app.settings.execution_routing);
	UAM_ASSERT(SettingsStore::Save(temp.root / "settings.txt", app.settings));
	AppSettings loaded;
	UAM_ASSERT(SettingsStore::Load(temp.root / "settings.txt", loaded).loaded);
	UAM_ASSERT_EQ(SettingsStore::SerializeExecutionRoutingSettings(loaded.execution_routing), original);
	std::string error;
	nlohmann::json invalid = original;
	invalid["hosts"][0]["maxActiveTurns"] = -1;
	UAM_ASSERT(!SettingsStore::ParseExecutionRoutingSettings(invalid, loaded.execution_routing, error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT_EQ(SettingsStore::SerializeExecutionRoutingSettings(loaded.execution_routing), original);
	invalid = original;
	invalid["hosts"].push_back(original["hosts"][0]);
	UAM_ASSERT(!SettingsStore::ParseExecutionRoutingSettings(invalid, loaded.execution_routing, error));
	UAM_ASSERT_EQ(SettingsStore::SerializeExecutionRoutingSettings(loaded.execution_routing), original);
}

UAM_TEST(ExecutionRoutingPersistsRemoteAssignmentWithExplicitSshWorktreeParity)
{
	TempDir temp("uam-routing-assignment");
	uam::AppState app;
	ConfigureRoutingFixture(app, temp.root);
	UAM_ASSERT_EQ(app.chats.front().workspace_isolation_kind, std::string(uam::paths::kPendingGitWorktreeIsolationKind));
	const uam::ExecutionRoutingDecision decision = uam::AdmitExecutionRouting(app, app.chats.front());
	UAM_ASSERT_EQ(decision.status, uam::ExecutionRoutingStatus::Assigned);
	UAM_ASSERT_EQ(app.chats.front().execution_host_id, std::string("remote"));
	UAM_ASSERT_EQ(app.chats.front().folder_id, std::string("destination"));
	UAM_ASSERT_EQ(app.chats.front().workspace_directory, std::string("/mapped/repo"));
	UAM_ASSERT(app.chats.front().workspace_worktree_directory.empty());
	UAM_ASSERT(!fs::exists(temp.root / "repo"));
	const std::optional<ChatSession> persisted = ChatRepository::LoadLocalChat(app.data_root, "automatic-chat");
	UAM_ASSERT(persisted.has_value());
	UAM_ASSERT_EQ(persisted->routing_mode, std::string("automatic"));
	UAM_ASSERT_EQ(persisted->routing_source_directory, (temp.root / "repo").string());
	UAM_ASSERT_EQ(persisted->execution_host_id, std::string("remote"));
	UAM_ASSERT(persisted->routing_reservation_pending);
	UAM_ASSERT(!persisted->routing_assigned_connection_identity.empty());
	UAM_ASSERT(app.acp_sessions.empty());
	UAM_ASSERT(app.cli_terminals.empty());
}

UAM_TEST(ExecutionRoutingMaterializedWorktreeWithoutExactMappingWaitsWithoutMutation)
{
	TempDir temp("uam-routing-worktree-mapping");
	uam::AppState app;
	ConfigureRoutingFixture(app, temp.root);
	app.chats.front().workspace_isolation_kind = uam::paths::kGitWorktreeIsolationKind;
	app.chats.front().workspace_worktree_directory = (temp.root / "repo-worktree").string();
	const uam::ExecutionRoutingDecision decision = uam::AdmitExecutionRouting(app, app.chats.front());
	UAM_ASSERT_EQ(decision.status, uam::ExecutionRoutingStatus::Waiting);
	UAM_ASSERT_EQ(app.chats.front().execution_host_id, std::string("local"));
	UAM_ASSERT_EQ(app.chats.front().folder_id, std::string("source"));
	UAM_ASSERT(app.chats.front().routing_assigned_connection_identity.empty());
	UAM_ASSERT(!fs::exists(AppPaths::UamChatFilePath(app.data_root, "automatic-chat")));
	UAM_ASSERT(!app.execution_routing_waits["automatic-chat"].empty());
}

UAM_TEST(ExecutionRoutingFailedAssignmentSaveReleasesOnlyPrelaunchReservation)
{
	TempDir temp("uam-routing-save-failure");
	uam::AppState app;
	ConfigureRoutingFixture(app, temp.root);
	std::ofstream blocked(app.data_root);
	blocked << "not-a-directory";
	blocked.close();
	const uam::ExecutionRoutingDecision failed = uam::AdmitExecutionRouting(app, app.chats.front());
	UAM_ASSERT_EQ(failed.status, uam::ExecutionRoutingStatus::Waiting);
	UAM_ASSERT_EQ(app.chats.front().execution_host_id, std::string("local"));
	UAM_ASSERT_EQ(app.chats.front().folder_id, std::string("source"));
	UAM_ASSERT(!app.chats.front().routing_reservation_pending);
	UAM_ASSERT(app.chats.front().routing_assigned_connection_identity.empty());
	app.data_root = temp.root / "repaired-data";
	app.execution_routing_retry_at_by_chat_id.clear();
	UAM_ASSERT_EQ(uam::AdmitExecutionRouting(app, app.chats.front()).status, uam::ExecutionRoutingStatus::Assigned);
}

UAM_TEST(ExecutionRoutingIdleNativeIdDoesNotOccupyButLiveTerminalDoes)
{
	TempDir temp("uam-routing-occupancy");
	uam::AppState app;
	ConfigureRoutingFixture(app, temp.root);
	ChatSession idle;
	idle.id = "idle-native";
	idle.execution_host_id = "remote";
	idle.native_session_id = "persisted-native";
	idle.remote_process_exists = true;
	app.chats.push_back(idle);
	UAM_ASSERT(!uam::ExecutionRoutingOwnsWork(app, app.chats.back()));
	std::unique_ptr<uam::CliTerminalState> terminal = std::make_unique<uam::CliTerminalState>();
	terminal->attached_chat_id = "idle-native";
	terminal->running = true;
	terminal->generation_in_progress = false;
	app.cli_terminals.push_back(std::move(terminal));
	UAM_ASSERT(uam::ExecutionRoutingOwnsWork(app, app.chats.back()));
	UAM_ASSERT_EQ(uam::AdmitExecutionRouting(app, app.chats.front()).status, uam::ExecutionRoutingStatus::Waiting);
	app.cli_terminals.front()->running = false;
	app.execution_routing_retry_at_by_chat_id.clear();
	UAM_ASSERT_EQ(uam::AdmitExecutionRouting(app, app.chats.front()).status, uam::ExecutionRoutingStatus::Assigned);
}

UAM_TEST(ExecutionRoutingRestartRestoresOutboxOwnershipButNotIdleNativeAssignments)
{
	TempDir temp("uam-routing-restart");
	uam::AppState original;
	ConfigureRoutingFixture(original, temp.root);
	const ChatSession fresh = original.chats.front();
	UAM_ASSERT_EQ(uam::AdmitExecutionRouting(original, original.chats.front()).status, uam::ExecutionRoutingStatus::Assigned);
	ChatSession idle = original.chats.front();
	idle.routing_reservation_pending = false;
	idle.native_session_id = "owned-native-id";
	idle.remote_process_exists = true;
	UAM_ASSERT(ChatRepository::SaveChat(original.data_root, idle));

	uam::AppState restored;
	ConfigureRoutingFixture(restored, temp.root);
	restored.chats.front() = idle;
	ChatSession next = fresh;
	next.id = "next-chat";
	restored.chats.push_back(next);
	UAM_ASSERT(uam::RestoreExecutionRoutingReservations(restored));
	UAM_ASSERT_EQ(uam::AdmitExecutionRouting(restored, restored.chats.back()).status, uam::ExecutionRoutingStatus::Assigned);

	uam::AppState uncertain;
	ConfigureRoutingFixture(uncertain, temp.root);
	idle.remote_prompt_delivery_id = "accepted-receipt-not-confirmed";
	idle.remote_prompt_delivery_payload = "exact-owned-payload";
	uncertain.chats.front() = idle;
	uncertain.chats.push_back(next);
	UAM_ASSERT(uam::RestoreExecutionRoutingReservations(uncertain));
	UAM_ASSERT_EQ(uam::AdmitExecutionRouting(uncertain, uncertain.chats.back()).status, uam::ExecutionRoutingStatus::Waiting);
	UAM_ASSERT_EQ(uncertain.chats.front().execution_host_id, std::string("remote"));
	UAM_ASSERT_EQ(uncertain.chats.front().remote_prompt_delivery_payload, std::string("exact-owned-payload"));
}

UAM_TEST(ExecutionRoutingUnavailableOrStaleProviderObservationCannotBeAdmitted)
{
	TempDir temp("uam-routing-provider-readiness");
	uam::AppState app;
	ConfigureRoutingFixture(app, temp.root);
	const std::string key = CliProviderVersionStateKey("codex-cli", "remote");
	uam::CliProviderVersionState& version = app.runtime_cli_versions_by_provider_id.at(key);
	version.checked = false;
	UAM_ASSERT_EQ(uam::AdmitExecutionRouting(app, app.chats.front()).status, uam::ExecutionRoutingStatus::Waiting);
	version.checked = true;
	version.supported = false;
	app.execution_routing_retry_at_by_chat_id.clear();
	UAM_ASSERT_EQ(uam::AdmitExecutionRouting(app, app.chats.front()).status, uam::ExecutionRoutingStatus::Waiting);
	version.supported = true;
	version.execution_host.ssh_alias = "old-connection";
	app.execution_routing_retry_at_by_chat_id.clear();
	UAM_ASSERT_EQ(uam::AdmitExecutionRouting(app, app.chats.front()).status, uam::ExecutionRoutingStatus::Waiting);
	version.execution_host = app.settings.execution_hosts.back();
	app.execution_routing_retry_at_by_chat_id.clear();
	UAM_ASSERT_EQ(uam::AdmitExecutionRouting(app, app.chats.front()).status, uam::ExecutionRoutingStatus::Assigned);
}

UAM_TEST(ExecutionRoutingDisabledProviderWaitsUntilHostReenabled)
{
	TempDir temp("uam-routing-provider-availability");
	uam::AppState app;
	ConfigureRoutingFixture(app, temp.root);
	UAM_ASSERT(uam::provider_availability::Set(app.settings, "codex", "remote", false));
	UAM_ASSERT_EQ(uam::AdmitExecutionRouting(app, app.chats.front()).status, uam::ExecutionRoutingStatus::Waiting);
	UAM_ASSERT_EQ(app.chats.front().execution_host_id, std::string("local"));
	UAM_ASSERT(uam::provider_availability::Set(app.settings, "codex", "remote", true));
	app.execution_routing_retry_at_by_chat_id.clear();
	UAM_ASSERT_EQ(uam::AdmitExecutionRouting(app, app.chats.front()).status, uam::ExecutionRoutingStatus::Assigned);
	UAM_ASSERT_EQ(app.chats.front().execution_host_id, std::string("remote"));
}

UAM_TEST(ExecutionRoutingUnknownRestartReservationRequiresDurableAcknowledgedRelease)
{
	TempDir temp("uam-routing-unknown-reservation");
	uam::AppState original;
	ConfigureRoutingFixture(original, temp.root);
	const ChatSession fresh = original.chats.front();
	const uam::ExecutionRoutingDecision assigned = uam::AdmitExecutionRouting(original, original.chats.front(), true);
	UAM_ASSERT_EQ(assigned.status, uam::ExecutionRoutingStatus::Assigned);
	uam::AppState restored;
	ConfigureRoutingFixture(restored, temp.root);
	restored.chats.front() = original.chats.front();
	ChatSession next = fresh;
	next.id = "next-chat";
	restored.chats.push_back(next);
	UAM_ASSERT(uam::RestoreExecutionRoutingReservations(restored));
	(void)uam::PollExecutionRouting(restored);
	UAM_ASSERT(restored.chats.front().routing_reservation_pending);
	UAM_ASSERT_EQ(uam::AdmitExecutionRouting(restored, restored.chats.front(), true).status, uam::ExecutionRoutingStatus::Waiting);
	UAM_ASSERT_EQ(uam::AdmitExecutionRouting(restored, restored.chats.back()).status, uam::ExecutionRoutingStatus::Waiting);
	std::string error;
	UAM_ASSERT(!uam::ReleaseUnresolvedExecutionRouting(restored, assigned.assignment, false, error));
	uam::ExecutionRoutingAssignment stale = assigned.assignment;
	stale.connection_identity = "stale";
	UAM_ASSERT(!uam::ReleaseUnresolvedExecutionRouting(restored, stale, true, error));
	std::unique_ptr<uam::CliTerminalState> terminal = std::make_unique<uam::CliTerminalState>();
	terminal->attached_chat_id = restored.chats.front().id;
	terminal->running = true;
	restored.cli_terminals.push_back(std::move(terminal));
	UAM_ASSERT(!uam::ReleaseUnresolvedExecutionRouting(restored, assigned.assignment, true, error));
	restored.cli_terminals.front()->running = false;
	const fs::path valid_data = restored.data_root;
	restored.data_root = temp.root / "blocked";
	std::ofstream blocked(restored.data_root);
	blocked << "not-a-directory";
	blocked.close();
	UAM_ASSERT(!uam::ReleaseUnresolvedExecutionRouting(restored, assigned.assignment, true, error));
	UAM_ASSERT(restored.chats.front().routing_reservation_pending);
	restored.data_root = valid_data;
	UAM_ASSERT(uam::ReleaseUnresolvedExecutionRouting(restored, assigned.assignment, true, error));
	const std::optional<ChatSession> persisted = ChatRepository::LoadLocalChat(restored.data_root, assigned.assignment.chat_id);
	UAM_ASSERT(persisted.has_value());
	UAM_ASSERT(!persisted->routing_reservation_pending);
	restored.execution_routing_retry_at_by_chat_id.clear();
	UAM_ASSERT_EQ(uam::AdmitExecutionRouting(restored, restored.chats.back()).status, uam::ExecutionRoutingStatus::Assigned);
}

UAM_TEST(ExecutionRoutingVerifiedReattachedOwnerSettlesAndReleasesReservation)
{
	TempDir temp("uam-routing-reattached-owner");
	uam::AppState original;
	ConfigureRoutingFixture(original, temp.root);
	const ChatSession fresh = original.chats.front();
	UAM_ASSERT_EQ(uam::AdmitExecutionRouting(original, original.chats.front()).status, uam::ExecutionRoutingStatus::Assigned);
	uam::AppState restored;
	ConfigureRoutingFixture(restored, temp.root);
	restored.chats.front() = original.chats.front();
	ChatSession next = fresh;
	next.id = "next-chat";
	restored.chats.push_back(next);
	UAM_ASSERT(uam::RestoreExecutionRoutingReservations(restored));
	std::unique_ptr<uam::AcpSessionState> session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = restored.chats.front().id;
	session->provider_id = restored.chats.front().provider_id;
	session->process_execution_host_id = "remote";
	session->running = session->session_ready = session->processing = true;
	restored.acp_sessions.push_back(std::move(session));
	UAM_ASSERT(uam::PollExecutionRouting(restored));
	UAM_ASSERT(!restored.execution_routing_restored_unresolved_chat_ids.contains(restored.chats.front().id));
	UAM_ASSERT(restored.chats.front().routing_reservation_pending);
	restored.acp_sessions.front()->processing = false;
	restored.execution_routing_next_poll_s = 0;
	UAM_ASSERT(uam::PollExecutionRouting(restored));
	UAM_ASSERT(!restored.chats.front().routing_reservation_pending);
	UAM_ASSERT_EQ(uam::AdmitExecutionRouting(restored, restored.chats.back()).status, uam::ExecutionRoutingStatus::Assigned);
}

UAM_TEST(ExecutionRoutingAssignedWorktreeKeepsRegisteredBaseOwnership)
{
	TempDir temp("uam-routing-assigned-worktree");
	uam::AppState app;
	ConfigureRoutingFixture(app, temp.root);
	UAM_ASSERT_EQ(uam::AdmitExecutionRouting(app, app.chats.front()).status, uam::ExecutionRoutingStatus::Assigned);
	app.chats.front().workspace_isolation_kind = uam::paths::kGitWorktreeIsolationKind;
	app.chats.front().workspace_worktree_directory = "/mapped/worktree";
	app.chats.front().routing_reservation_pending = false;
	app.execution_routing_service = std::make_shared<uam::ExecutionRoutingService>();
	app.execution_routing_reserved_chat_ids.clear();
	UAM_ASSERT_EQ(uam::AdmitExecutionRouting(app, app.chats.front()).status, uam::ExecutionRoutingStatus::Assigned);
	UAM_ASSERT_EQ(app.chats.front().workspace_directory, std::string("/mapped/repo"));
	UAM_ASSERT_EQ(app.chats.front().workspace_worktree_directory, std::string("/mapped/worktree"));
}

UAM_TEST(ExecutionRoutingMcpChangesInvalidateHostConfirmation)
{
	for (int variation = 0; variation < 3; ++variation)
	{
		TempDir temp("uam-routing-mcp-identity");
		uam::AppState app;
		ConfigureRoutingFixture(app, temp.root);
		McpServerConfiguration server;
		server.id = "private-fixture";
		server.execution_host_id = "remote";
		server.command = "must-not-start";
		server.environment.push_back({"PRIVATE_FIXTURE", "", "before"});
		app.settings.mcp_servers.push_back(server);
		const std::string confirmed = uam::ExecutionRoutingProviderIdentity(app, app.provider_profiles.front());
		app.settings.execution_routing.hosts.front().provider_confirmations.front().provider_configuration_identity = confirmed;
		if (variation == 0) app.settings.mcp_servers.front().command = "must-not-start-changed";
		else if (variation == 1) app.settings.mcp_servers.front().environment.front().value = "after";
		else app.settings.mcp_servers.front().enabled = false;
		const std::string changed = uam::ExecutionRoutingProviderIdentity(app, app.provider_profiles.front());
		UAM_ASSERT(changed != confirmed);
		UAM_ASSERT_EQ(uam::AdmitExecutionRouting(app, app.chats.front()).status, uam::ExecutionRoutingStatus::Waiting);
		UAM_ASSERT_EQ(app.chats.front().execution_host_id, std::string("local"));
		app.settings.execution_routing.hosts.front().provider_confirmations.front().provider_configuration_identity = changed;
		app.execution_routing_retry_at_by_chat_id.clear();
		UAM_ASSERT_EQ(uam::AdmitExecutionRouting(app, app.chats.front()).status, uam::ExecutionRoutingStatus::Assigned);
		UAM_ASSERT_EQ(app.chats.front().execution_host_id, std::string("remote"));
	}
}
