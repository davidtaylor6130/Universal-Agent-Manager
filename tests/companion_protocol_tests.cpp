#include "common/companion/protocol.h"
#include "common/utils/base64.h"
#include "cef/uam_bridge_request.h"

#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <thread>

namespace
{
	using namespace uam::companion;
	using Json = nlohmann::json;
	using Clock = RequestTransfers::Clock;
	const Clock::time_point kNow{};

	void Require(bool condition, const char* message)
	{
		if (!condition)
			throw std::runtime_error(message);
	}

	RequestTransfers Transfers()
	{
		return RequestTransfers([next = 0]() mutable { return "server-uuid-" + std::to_string(++next); });
	}

	void TransferRoundTripCommitOnce()
	{
		auto transfers = Transfers();
		const std::string original = Json{{"action", "sendAcpPrompt"}, {"payload", {{"chatId", "chat-one"}, {"text", std::string(90 * 1024, 'x') + " 📱"}}}}.dump();
		const auto begun = transfers.Begin(original.size(), kNow);
		Require(begun.Ok() && begun.id == "server-uuid-1", "Server must own the transfer identity");
		for (std::size_t offset = 0; offset < original.size();)
		{
			const std::string bytes = original.substr(offset, kRequestChunkBytes);
			const auto appended = transfers.Append(begun.id, offset, uam::base64::Encode(bytes), kNow);
			Require(appended.Ok() && appended.next_offset == offset + bytes.size(), "UTF-8 envelope chunks must round trip by byte offset");
			offset = appended.next_offset;
		}
		const auto committed = transfers.Commit(begun.id, kNow);
		Require(committed.Ok() && committed.body == original, "Commit must return the original request envelope");
		auto parsed = uam::cef::ParseBridgeRequest(committed.body);
		Require(parsed.ok && PrepareRequest(parsed.request.action, parsed.request.payload).Ok(), "Committed request must pass the existing parser and companion policy");
		Require(transfers.Commit(begun.id, kNow).status == 404, "A duplicate commit must not dispatch twice");
		Require(transfers.Append(begun.id, 0, "eA==", kNow).status == 404, "Committed uploads cannot be revived");
	}

	void InFlightRequestAdmissionSurvivesAsyncOwnership()
	{
		RequestAdmission admission;
		auto lease = admission.Acquire(kMaxRequestBytes);
		Require(lease != nullptr && !admission.Acquire(1), "A committed envelope must keep aggregate admission capacity reserved");
		auto callback_lease = lease;
		lease.reset();
		Require(!admission.Acquire(1), "Queue dispatch cannot release capacity still owned by an asynchronous callback");
		std::thread callback([lease = std::move(callback_lease)]() mutable { lease.reset(); });
		callback.join();
		Require(admission.Acquire(kMaxRequestBytes) != nullptr, "Callback completion releases the reservation across threads");
		std::vector<std::shared_ptr<RequestAdmission::Lease>> queued;
		for (std::size_t index = 0; index < kMaxInFlightRequests; ++index)
		{
			queued.push_back(admission.Acquire(1));
			Require(queued.back() != nullptr, "Bounded concurrent actions are admitted");
		}
		Require(!admission.Acquire(1), "Small requests are bounded by in-flight count as well as bytes");
		queued.clear();
		Require(!admission.Acquire(0) && !admission.Acquire(kMaxRequestBytes + 1), "Empty and oversized admissions fail without corrupting capacity");
		Require(admission.Acquire(kMaxRequestBytes) != nullptr, "Failed admission preserves the reusable aggregate budget");
	}

	void ResponseTransferAggregateBoundsAndFinalRelease()
	{
		ResponseTransfers responses([id = 0]() mutable { return "response-" + std::to_string(++id); });
		Require(!responses.Store(std::string(kMaxResponseBytes + 1, 'x'), kNow), "An oversized response cannot enter retained storage");
		const auto stored = responses.Store(std::string(kMaxResponseBytes, 'x'), kNow);
		Require(stored && stored->total_bytes == kMaxResponseBytes && !responses.Store("x", kNow), "Response aggregate capacity cannot exceed32MiB even with unused entry slots");
		Require(!responses.Read(stored->id, kMaxResponseBytes, kNow), "Out-of-range reads cannot consume a retained response");
		std::size_t offset = 0;
		while (offset < kMaxResponseBytes)
		{
			const auto chunk = responses.Read(stored->id, offset, kNow);
			Require(chunk && chunk->bytes == std::string(kResponseChunkBytes, 'x') && chunk->next_offset == offset + kResponseChunkBytes, "Stored responses retain exact bounded chunk content and offsets");
			offset = chunk->next_offset;
			Require(chunk->done == (offset == kMaxResponseBytes), "Only the exact final response chunk consumes storage");
		}
		Require(!responses.Read(stored->id, 0, kNow) && responses.Store(std::string(kMaxResponseBytes, 'y'), kNow).has_value(), "Final-chunk consumption releases aggregate bytes for the next response");
		responses.Clear();
		for (std::size_t index = 0; index < kMaxResponseTransfers; ++index) Require(responses.Store("x", kNow).has_value(), "Small stored responses fit the entry budget");
		Require(!responses.Store("x", kNow), "Response entry count remains bounded independently of total bytes");
	}

	void ResponseTransferTimerExpiryAndIdentity()
	{
		ResponseTransfers responses([id = 0]() mutable { return "response-" + std::to_string(++id); });
		const auto abandoned = responses.Store(std::string(kMaxResponseBytes, 'x'), kNow);
		Require(abandoned.has_value(), "Maximum retained response is accepted");
		responses.Expire(kNow + kResponseTransferTtl);
		Require(!responses.Read(abandoned->id, 0, kNow + kResponseTransferTtl), "Timer expiry removes an abandoned response without another store or chunk read");
		const auto touched = responses.Store(std::string(kResponseChunkBytes + 1, 'z'), kNow);
		Require(touched && responses.Read(touched->id, 0, kNow + std::chrono::seconds(119)), "A valid intermediate chunk refreshes inactivity expiry");
		responses.Expire(kNow + std::chrono::seconds(120));
		const auto final = responses.Read(touched->id, kResponseChunkBytes, kNow + std::chrono::seconds(238));
		Require(final && final->done && final->bytes == "z", "A recently touched response remains available through its final chunk");
		ResponseTransfers collision([] { return "same-id"; });
		const auto original = collision.Store("original", kNow);
		Require(original && !collision.Store("replacement", kNow) && collision.Read(original->id, 0, kNow)->bytes == "original", "An ID collision cannot replace an existing retained body");
		ResponseTransfers missing_id([] { return std::string{}; });
		Require(!missing_id.Store("x", kNow), "Empty generated response identities are rejected");
	}

	void TransferMalformedOffsets()
	{
		auto transfers = Transfers();
		const auto transfer = transfers.Begin(4, kNow);
		Require(transfers.Append(transfer.id, 1, "YWI=", kNow).status == 409, "Out-of-order first chunk must fail");
		Require(transfers.Append(transfer.id, std::numeric_limits<std::size_t>::max(), "YWI=", kNow).status == 409, "Huge offsets must not underflow bounds");
		Require(transfers.Append(transfer.id, 0, "YWI=", kNow).next_offset == 2, "First chunk must advance exactly");
		Require(transfers.Append(transfer.id, 0, "YWI=", kNow).status == 409, "Duplicate chunks must not be silently appended");
		Require(transfers.Append(transfer.id, 3, "Yw==", kNow).status == 409, "A gap must fail");
		Require(transfers.Append(transfer.id, 2, "Y2Q=", kNow).Ok(), "Rejected chunks must leave the contiguous prefix intact");
		Require(transfers.Commit(transfer.id, kNow).body == "abcd", "Only valid bytes may be retained");
	}

	void TransferStrictBase64AndBounds()
	{
		auto transfers = Transfers();
		const auto transfer = transfers.Begin(2, kNow);
		for (const char* invalid : {"", "A", "YQ", "YQ=", "YR==", "YWJ=", "Y Q==", "YQ==\n", "YQ-_", "====", "Y=Q=", "YQ==AAAA"})
			Require(transfers.Append(transfer.id, 0, invalid, kNow).status == 400, "Malformed or noncanonical base64 must fail");
		Require(transfers.Append(transfer.id, 0, "YWJj", kNow).status == 413, "Chunks cannot exceed the declared request size");
		const std::string huge = uam::base64::Encode(std::string(kRequestChunkBytes + 1, 'x'));
		Require(transfers.Append(transfer.id, 0, huge, kNow).status == 413, "Decoded chunks cannot exceed 32 KiB");
		Require(transfers.Append(transfer.id, 0, "YWI=", kNow).Ok(), "Rejected chunks must not corrupt the upload");
		Require(transfers.Commit(transfer.id, kNow).body == "ab", "Valid canonical base64 must decode");
	}

	void TransferOversizeCapacityAndRelease()
	{
		auto transfers = Transfers();
		Require(transfers.Begin(0, kNow).status == 413, "Empty transfers must fail");
		Require(transfers.Begin(kMaxRequestBytes + 1, kNow).status == 413, "Requests over 32 MiB must fail before allocation");
		const auto maximum = transfers.Begin(kMaxRequestBytes, kNow);
		Require(maximum.Ok() && transfers.Begin(1, kNow).status == 503, "Declared bytes across transfers must be bounded");
		Require(transfers.Commit(maximum.id, kNow).status == 409, "Incomplete commit must fail and consume the transfer");
		Require(transfers.Commit(maximum.id, kNow).status == 404, "Failed incomplete commits are single use");
		for (std::size_t i = 0; i < kMaxRequestTransfers; ++i)
			Require(transfers.Begin(1, kNow).Ok(), "Released reservations must be reusable");
		Require(transfers.Begin(1, kNow).status == 503, "Concurrent transfer count must be bounded");
		transfers.Clear();
		Require(transfers.Begin(kMaxRequestBytes, kNow).Ok(), "Shutdown clearing must release all capacity");
	}

	void TransferAbsoluteExpiry()
	{
		auto transfers = Transfers();
		const auto transfer = transfers.Begin(2, kNow);
		Require(transfers.Append(transfer.id, 0, "YQ==", kNow + std::chrono::seconds(119)).Ok(), "Transfer remains available before TTL");
		Require(transfers.Append(transfer.id, 1, "Yg==", kNow + kRequestTransferTtl).status == 404, "Appending cannot extend the absolute TTL");
		Require(transfers.Commit(transfer.id, kNow + kRequestTransferTtl).status == 404, "Expired bytes must never dispatch");
		Require(transfers.Begin(kMaxRequestBytes, kNow + kRequestTransferTtl).Ok(), "Expiry must release byte reservations");
	}

	void TransferIdCollision()
	{
		RequestTransfers transfers([] { return "same-server-id"; });
		const auto original = transfers.Begin(1, kNow);
		Require(transfers.Begin(1, kNow).status == 503, "UUID collisions cannot overwrite an existing request");
		Require(transfers.Append(original.id, 0, "eA==", kNow).Ok() && transfers.Commit(original.id, kNow).body == "x", "Collision must preserve the original upload");
	}

	void TransferEnvelopeFieldValidation()
	{
		for (const char* raw : {"{}", "{\"totalBytes\":-1}", "{\"totalBytes\":true}", "{\"totalBytes\":1.5}", "{\"totalBytes\":\"1\"}", "{\"totalBytes\":1,\"id\":\"client-id\"}"})
			Require(ValidateTransferPayload("beginCompanionRequestTransfer", Json::parse(raw)).status == 400, "Begin must reject malformed types and client-selected ids");
		Require(ValidateTransferPayload("beginCompanionRequestTransfer", Json::parse("{\"totalBytes\":33554432}")).Ok(), "Maximum unsigned request size is valid");
		Require(ValidateTransferPayload("beginCompanionRequestTransfer", Json::parse("{\"totalBytes\":33554433}")).status == 413, "Oversized totals must fail before allocation");
		for (const char* raw : {"{\"id\":\"server-id\",\"offset\":-1,\"base64\":\"eA==\"}", "{\"id\":\"server-id\",\"offset\":0.0,\"base64\":\"eA==\"}", "{\"id\":\"server-id\",\"offset\":false,\"base64\":\"eA==\"}", "{\"id\":\"server-id\",\"offset\":18446744073709551615,\"base64\":\"eA==\"}", "{\"id\":\"server-id\",\"offset\":0,\"base64\":[],\"extra\":1}"})
			Require(ValidateTransferPayload("appendCompanionRequestChunk", Json::parse(raw)).status == 400, "Wire offsets must be bounded unsigned integers");
		const Json commit = {{"id", "server-id"}};
		Require(ValidateTransferPayload("commitCompanionRequestTransfer", commit).Ok(), "Commit accepts only the server-issued id");
		Require(ValidateTransferPayload("commitCompanionRequestTransfer", commit, true).status == 403, "Transfer controls cannot nest inside a transferred envelope");
		Require(ValidateTransferPayload("commitCompanionRequestTransfer", {{"id", "server-id"}, {"extra", 1}}).status == 400, "Commit must reject unknown payload fields");
	}

	void PolicyCapabilitiesAndRestrictions()
	{
		const Json state = {{"bootId", "host-instance"}, {"settings", {{"executionHosts", Json::array({{{"id", "local"}, {"label", "Main computer"}, {"transport", "local"}, {"sshAlias", "private-alias"}}})}}}};
		const auto capabilities = Capabilities(state);
		Require(capabilities["protocolVersion"] == 1 && capabilities["mainHost"]["id"] == "local" && capabilities["mainHost"]["instanceId"] == "host-instance", "Capabilities must identify the protocol and main host instance");
		Require(capabilities["features"]["chatDelete"] == true && capabilities["features"]["chatBranch"] == true, "Capabilities advertise guarded lifecycle operations");
		Require(!capabilities["mainHost"].contains("sshAlias"), "Host identity must not leak connection configuration");
		Require(capabilities["restrictions"]["settingsReadOnly"] == false && capabilities["restrictions"]["configurationDefinitionsReadOnly"] == true && capabilities["restrictions"]["connectionAdministrationReadOnly"] == true && capabilities["restrictions"]["desktopSelectionReadOnly"] == true && capabilities["restrictions"]["automaticReplay"] == false, "Capabilities must explicitly declare restricted ownership and no replay");
		for (const auto& entry : capabilities["actions"])
			Require(FindAction(entry["action"].get<std::string>()) || IsRequestTransferAction(entry["action"].get<std::string>()), "Every advertised action must exist in the companion policy");
		for (const char* action : {"getCompanionToken", "setCompanionEnabled", "setMcpServers", "setCentralProviderConfiguration", "writeClipboardText", "selectSession", "setVisibleChatIds", "releaseChatMessages", "deleteSessions", "applyOrganizationAction", "startCli", "saveAgentDefinition", "notAnAction"})
		{
			Json payload = {{"chatId", "chat-one"}};
			Require(PrepareRequest(action, payload).status == 403, "Dangerous/global/selection actions must remain unavailable");
		}
		for (const char* action : {"getUsageHistory", "getProviderAllowances", "renameSession", "setChatSnoozedUntil", "setChatSettled", "listUamAgents", "stageChatAttachments", "deleteSession", "branchFromMessage"})
			Require(FindAction(action) != nullptr, "Safe mobile actions must be explicitly present");
	}

	void PolicyScopeSelectionAndReadOnly()
	{
		Json payload = Json::object();
		Require(PrepareRequest("setChatModel", payload).status == 400, "Controls cannot implicitly target desktop selection");
		payload = {{"chatId", "   "}};
		Require(PrepareRequest("setChatModel", payload).status == 400, "Blank chat identity must fail");
		for (const char* action : {"createSession", "openNativeSessionChat", "branchFromMessage"})
		{
			payload = {{"chatId", "chat-one"}, {"messageIndex", 0}, {"selectChat", true}};
			Require(PrepareRequest(action, payload).Ok() && payload["selectChat"] == false, "Companion must override desktop selection acquisition");
		}
		payload = {{"chatId", "chat-one"}};
		Require(PrepareRequest("deleteSession", payload).Ok(), "Single deletion is explicitly scoped");
		payload["chatIds"] = Json::array({"other"});
		Require(PrepareRequest("deleteSession", payload).status == 400, "Single deletion cannot carry bulk targets");
		for (const auto& index : {Json(-1), Json(0.5), Json("0"), Json(std::uint64_t(1) << 63)})
		{
			payload = {{"chatId", "chat-one"}, {"messageIndex", index}};
			Require(PrepareRequest("branchFromMessage", payload).status == 400, "Invalid branch indices fail before dispatch");
		}
		payload = {{"chatId", "chat-one"}, {"messageIndex", 0}, {"regenerate", false}, {"operationId", "mobile-branch"}, {"retainedAttachmentIds", Json::array({"image-id"})}};
		Require(PrepareRequest("branchFromMessage", payload).Ok() && payload["selectChat"] == false && payload["regenerate"] == false, "Companion branch preserves explicit creation-only and media controls");
		payload = {{"summaryOnly", false}};
		Require(PrepareRequest("getInitialState", payload).Ok() && payload["summaryOnly"] == true, "Companion initial state must stay summary-only");
		payload = {{"sourceChatId", "chat-one"}, {"selectChat", false}};
		Require(PrepareRequest("createSession", payload).status == 403, "Async shared-worktree creation must not restore stale desktop selection");
		const Json state = {{"chats", Json::array({{{"id", "chat-one"}, {"importedReadOnly", true}}})}};
		payload = {{"chatId", "chat-one"}};
		for (const char* action : {"renameSession", "deleteSession", "branchFromMessage"})
			Require(AuthorizeChat(action, payload, state).status == 403, "Read-only imports cannot be mutated");
		Require(AuthorizeChat("getChatMessages", payload, state).Ok(), "Read-only imports remain readable");
		payload["chatId"] = "missing";
		Require(AuthorizeChat("sendAcpPrompt", payload, state).status == 404, "Controls must resolve the explicit chat");

		// The shared browser editor sends an explicit captured-skill selection,
		// including [] to remove all target skills. This is not new host attachment
		// authority: the native branch handler must still prove source membership,
		// uniqueness and the original captured prompt snapshot before publication.
		const Json branch = {{"chatId", "chat-one"}, {"messageIndex", 0}, {"content", "Edited delivered input"},
		    {"operationId", "browser-retained-skill-branch"}, {"retainedAttachmentIds", Json::array()}};
		for (const Json& invalid : {Json(), Json("/skills/retained.md"), Json::object(), Json::array({7}), Json::array({""}), Json::array({std::string("/skills/a") + '\0' + "b.md"}), Json::array({std::string(16385, 'x')})})
		{
			Json malformed = branch; malformed["retainedMarkdownStoreFiles"] = invalid;
			Require(PrepareRequest("branchFromMessage", malformed).status == 400, "Captured skill selections require bounded arrays of nonempty file identities");
		}
		Json too_many = branch;
		too_many["retainedMarkdownStoreFiles"] = Json::array();
		for (int index = 0; index < 129; ++index) too_many["retainedMarkdownStoreFiles"].push_back("/skills/" + std::to_string(index) + ".md");
		Require(PrepareRequest("branchFromMessage", too_many).status == 400, "Captured skill selection retains the native 128-item bound");
		for (const char* forbidden : {"retainedMarkdownStorePromptBlocks", "markdownStoreFiles", "unknownBranchAuthority"})
		{
			Json forged = branch; forged[forbidden] = Json::array({"/private/new-host-skill.md"});
			Require(PrepareRequest("branchFromMessage", forged).status == 400, "Branching cannot supply new host-skill attachment authority or fabricated prompt snapshots");
		}
		Json attach = {{"chatId", "chat-one"}, {"text", "Read"}, {"markdownStoreFiles", Json::array({"/private/new-host-skill.md"})}};
		Require(PrepareRequest("sendAcpPrompt", attach).status == 403, "Retained branch context must not widen ordinary companion host-skill attachment permission");
		bool captured_selection_allowed = true;
		for (const Json& selected : {Json::array({"/skills/retained.md"}), Json::array()})
		{
			Json explicit_selection = branch; explicit_selection["retainedMarkdownStoreFiles"] = selected;
			captured_selection_allowed = PrepareRequest("branchFromMessage", explicit_selection).Ok() && captured_selection_allowed;
			Require(explicit_selection.at("retainedMarkdownStoreFiles") == selected && explicit_selection.at("selectChat") == false, "Transport preserves captured skill selection and never acquires desktop selection");
			Require(AuthorizeChat("branchFromMessage", explicit_selection, state).status == 403, "Captured skill context cannot make imported history writable");
			const std::string body = Json{{"action", "branchFromMessage"}, {"payload", explicit_selection}}.dump();
			auto transfers = Transfers();
			const auto begun = transfers.Begin(body.size(), kNow);
			Require(begun.Ok() && transfers.Append(begun.id, 0, uam::base64::Encode(body), kNow).Ok(), "Captured branch context traverses real bounded request transfer");
			const auto committed = transfers.Commit(begun.id, kNow);
			auto parsed = uam::cef::ParseBridgeRequest(committed.body);
			Require(parsed.ok, "Captured branch selection remains a valid native bridge envelope");
			captured_selection_allowed = PrepareRequest(parsed.request.action, parsed.request.payload).Ok() && captured_selection_allowed;
		}
		Require(captured_selection_allowed, "Shared browser delivered edits must preserve explicit retained skill selection, including empty selection, through direct and uploaded branch policy");
	}

	Json QueueState()
	{
		return {{"chats", Json::array({{{"id", "chat-one"}, {"importedReadOnly", false}, {"acpSession", {{"queueHeld", false},
		    {"queuedPrompts", Json::array({{{"id", "first"}, {"revision", 1}, {"prepared", false}}, {{"id", "target"}, {"revision", 3}, {"prepared", false}}})},
		    {"pendingDirectPrompts", Json::array()}, {"unsentDirectPrompts", Json::array()}}}}})}};
	}

	Json QueueAction(const char* operation)
	{
		return {{"chatId", "chat-one"}, {"operation", operation}, {"index", 1}, {"expected", {{"id", "target"}, {"revision", 3}}}};
	}

	void PolicyQueueStableIdentityAndHeldState()
	{
		Json state = QueueState();
		Json payload = QueueAction("remove");
		Require(PrepareRequest("manageQueuedAcpPrompt", payload).Ok() && AuthorizeChat("manageQueuedAcpPrompt", payload, state).Ok(), "A current visible stable queue identity is removable");
		state["chats"][0]["acpSession"]["queuedPrompts"].erase(0);
		Require(AuthorizeChat("manageQueuedAcpPrompt", payload, state).Ok(), "A changed displayed index cannot invalidate the same stable ID and revision");
		payload["operation"] = "steer";
		Require(AuthorizeChat("manageQueuedAcpPrompt", payload, state).Ok(), "The same current stable identity is steerable");
		state["chats"][0]["acpSession"]["queueHeld"] = true;
		Require(AuthorizeChat("manageQueuedAcpPrompt", payload, state).status == 409, "A restored held queue cannot be steered");
		payload["operation"] = "remove";
		Require(AuthorizeChat("manageQueuedAcpPrompt", payload, state).Ok(), "Holding a queue does not prevent removal of a current visible row");
		state["chats"][0]["importedReadOnly"] = true;
		Require(AuthorizeChat("manageQueuedAcpPrompt", payload, state).status == 403, "Imported chat ownership still takes precedence over queue mutation");
	}

	void PolicyQueueAdmissionTransitionAndStaleIdentity()
	{
		for (const char* operation : {"remove", "steer"})
		{
			const Json payload = QueueAction(operation);
			for (const char* receipts : {"pendingDirectPrompts", "unsentDirectPrompts"})
			{
				Json state = QueueState();
				Require(AuthorizeChat("manageQueuedAcpPrompt", payload, state).Ok(), "A future row starts as a valid queue action");
				auto& session = state["chats"][0]["acpSession"];
				session[receipts].push_back(session["queuedPrompts"][1]);
				session["queuedPrompts"].erase(1);
				Require(AuthorizeChat("manageQueuedAcpPrompt", payload, state).status == 409, "Transition to direct admission must reject the old action even when ID/revision did not change");
			}
			Json state = QueueState();
			auto& row = state["chats"][0]["acpSession"]["queuedPrompts"][1];
			row["prepared"] = true;
			Require(AuthorizeChat("manageQueuedAcpPrompt", payload, state).status == 409, "A prepared row must reject even if accidentally left in the visible projection");
			row["prepared"] = false;
			row["revision"] = 4;
			Require(AuthorizeChat("manageQueuedAcpPrompt", payload, state).status == 409, "An edited row cannot be mutated using a stale revision");
			row["revision"] = 3;
			state["chats"][0]["acpSession"]["queuedPrompts"].push_back(row);
			Require(AuthorizeChat("manageQueuedAcpPrompt", payload, state).status == 409, "Ambiguous duplicate stable IDs fail closed");
		}
	}

	void PolicyQueueRequiresBoundedIdentity()
	{
		for (const char* operation : {"remove", "steer"})
		{
			Json payload = QueueAction(operation);
			payload.erase("expected");
			Require(PrepareRequest("manageQueuedAcpPrompt", payload).status == 400, "Companion cannot use the native legacy index-only mutation path");
			for (const Json& revision : {Json(0), Json(-1), Json(0.5), Json("3"), Json(std::uint64_t(1) << 63)})
			{
				payload = QueueAction(operation);
				payload["expected"]["revision"] = revision;
				Require(PrepareRequest("manageQueuedAcpPrompt", payload).status == 400, "Identity revision must be a positive native-range integer");
			}
			payload = QueueAction(operation);
			payload["expected"]["id"] = " ";
			Require(PrepareRequest("manageQueuedAcpPrompt", payload).status == 400, "Blank stable IDs cannot authorize queue mutations");
		}
		Json unknown = QueueAction("retry");
		Require(PrepareRequest("manageQueuedAcpPrompt", unknown).status == 400, "Unknown queue operations do not widen companion access");
	}

	void PolicyQueueExistingEditAndReorderOwnership()
	{
		Json state = QueueState();
		Json edit = {{"chatId", "chat-one"}, {"operation", "edit"}, {"promptId", "target"}, {"expectedRevision", 3}, {"text", "edited"}};
		Json reorder = {{"chatId", "chat-one"}, {"operation", "reorder"}, {"expected", Json::array({{{"id", "first"}, {"revision", 1}}, {{"id", "target"}, {"revision", 3}}})}, {"orderedIds", Json::array({"target", "first"})}};
		Require(PrepareRequest("manageQueuedAcpPrompt", edit).Ok() && AuthorizeChat("manageQueuedAcpPrompt", edit, state).Ok(), "Existing revision-bound edit stays available for visible input");
		Require(PrepareRequest("manageQueuedAcpPrompt", reorder).Ok() && AuthorizeChat("manageQueuedAcpPrompt", reorder, state).Ok(), "Existing complete-snapshot reorder stays available");
		auto& session = state["chats"][0]["acpSession"];
		session["pendingDirectPrompts"].push_back(session["queuedPrompts"][1]);
		session["queuedPrompts"].erase(1);
		Require(AuthorizeChat("manageQueuedAcpPrompt", edit, state).status == 409 && AuthorizeChat("manageQueuedAcpPrompt", reorder, state).status == 409, "Neither edit nor reorder may target a row that became native-owned delivery");
		Json discard = {{"chatId", "chat-one"}, {"operation", "discardUnsent"}, {"messageIndex", 2}, {"expectedMessagesDigest", "0123456789abcdef"}};
		Require(PrepareRequest("manageQueuedAcpPrompt", discard).Ok() && AuthorizeChat("manageQueuedAcpPrompt", discard, state).Ok(), "Existing unsent transcript discard retains its separate native digest guard");
	}

	void PolicySettingsRedaction()
	{
		const Json state = {{"unchanged", true}, {"bootId", "instance"}, {"stateRevision", 2}};
		Require(MobileState(state) == state, "Revision-polling unchanged responses must stay intact");
		const Json settings = {{"theme", "dark"}, {"mcpServers", "secret"}, {"centralProviderConfiguration", "secret"}, {"unknownFutureSecret", "secret"}, {"voiceInputServerBaseUrl", "private"}, {"executionHosts", Json::array({{{"id", "remote"}, {"label", "Remote"}, {"transport", "ssh"}, {"sshAlias", "private"}, {"instructionText", "secret"}, {"runnerDirectory", "private"}}})}, {"hostScopedSettingsValues", {{"remote", {{"defaultNewChatProviderId", "codex-cli"}, {"mcpServers", "secret"}}}}}};
		Json raw_state = {{"memoryActivity", {{"entryCount", 3}, {"runningCount", 0}, {"lastWorkerHasExitCode", true}, {"lastWorkerExitCode", 0}, {"lastStatus", "secret status"}, {"lastWorkerStatus", "secret status"}, {"lastWorkerOutput", "secret memory"}, {"lastWorkerError", "secret error"}}}};
		const auto projected_state = MobileState(raw_state);
		Require(projected_state.dump().find("secret") == std::string::npos && projected_state["memoryActivity"]["entryCount"] == 3 && projected_state["memoryActivity"]["status"] == "completed", "Initial-state memory activity must retain structured facts without raw extraction logs");
		const auto safe = MobileSettings(settings);
		Require(safe["theme"] == "dark" && safe["executionHosts"][0]["id"] == "remote", "Mobile must retain display settings and host identity");
		Require(safe.dump().find("secret") == std::string::npos && safe.dump().find("private") == std::string::npos, "Mobile settings must not expose credentials, instructions or unknown configuration");
		Require(safe["hostScopedSettingsValues"]["remote"]["defaultNewChatProviderId"] == "codex-cli", "Safe host defaults must survive projection");
	}

	Json SettingsState()
	{
		return {{"settings", {
		    {"executionHosts", Json::array({{{"id", "local"}, {"transport", "local"}}, {{"id", "remote"}, {"transport", "ssh"}}})},
		    {"providerChatDefaults", {{"codex-cli", {{"modelId", "preserved-model"}, {"memoryLevel", "off"}, {"memoryEnabled", false}, {"reasoningEffort", "high"}}}}},
		    {"hostScopedSettingsValues", {{"remote", {{"providerChatDefaults", {{"codex-cli", {{"memoryLevel", "readOnly"}, {"memoryEnabled", true}}}}}}}}},
		    {"memoryEnabledDefault", false}, {"memoryLevelDefault", "off"},
		    {"memoryWorkerBindings", {{"codex-cli", {{"workerProviderId", "opencode-cli"}, {"workerModelId", "inferdeck/normal"}}}}},
		    {"favoriteUamAgentIds", Json::array({"reviewer"})}, {"uamAgentCycleShortcut", "alt+shift+tab"},
		    {"gitEnabled", true}, {"svnEnabled", false}, {"backupLogRetentionDays", 14}, {"httpCacheClearIntervalDays", 7}
		}}};
	}

	void SettingsProviderDefaultsPartialAndScoped()
	{
		const auto state = SettingsState();
		for (const char* id : uam::provider_ids::kAllCliProviderIds)
		{
			Json payload = {{"defaults", {{id, {{"modelId", "provider/model"}}}}}};
			Require(PrepareRequest("setProviderChatDefaults", payload).Ok(), "All six provider defaults remain editable");
			const auto original = payload;
			Require(PrepareSettingsDispatch("setProviderChatDefaults", payload, state).Ok() && payload == original, "A model-only default patch must leave every omitted default to its native fallback");
		}
		Json payload = {{"defaults", {{"codex-cli", {{"memoryLevel", "balanced"}}}}}};
		Require(PrepareSettingsDispatch("setProviderChatDefaults", payload, state).Ok() && payload["defaults"]["codex-cli"]["memoryEnabled"] == true, "Level-only defaults can enable memory from off");
		payload = {{"scopeExecutionHostId", "remote"}, {"defaults", {{"codex-cli", {{"memoryEnabled", true}}}}}};
		Require(PrepareSettingsDispatch("setProviderChatDefaults", payload, state).Ok() && payload["defaults"]["codex-cli"]["memoryLevel"] == "readOnly", "A scoped memory enable keeps its environment's concrete level");
		payload = {{"defaults", {{"codex-cli", {{"memoryEnabled", true}}}}}};
		Require(PrepareSettingsDispatch("setProviderChatDefaults", payload, state).Ok() && payload["defaults"]["codex-cli"]["memoryLevel"] == "default", "Enabling an off provider default restores inheritance");
		for (const char* host : {"missing", "local"})
		{
			payload = {{"scopeExecutionHostId", host}, {"defaultProviderId", "codex-cli"}};
			Require(PrepareSettingsDispatch("setProviderChatDefaults", payload, state).status == 404, "Default scopes cannot invent computers or a local override");
		}
	}

	void SettingsMemoryAndWorkerPartialUpdates()
	{
		const auto state = SettingsState();
		Json payload = {{"levelDefault", "balanced"}};
		Require(PrepareSettingsDispatch("setMemorySettings", payload, state).Ok() && payload["enabledDefault"] == true && payload.size() == 2, "A memory level patch must enable from off without resetting budgets or workers");
		payload = {{"enabledDefault", true}};
		Require(PrepareSettingsDispatch("setMemorySettings", payload, state).Ok() && payload["levelDefault"] == "strict", "A boolean enable from off uses the native strict baseline");
		payload = {{"workerBindings", {{"codex-cli", {{"workerModelId", "new-model"}}}}}};
		Require(PrepareSettingsDispatch("setMemorySettings", payload, state).Ok() && payload["workerBindings"]["codex-cli"]["workerProviderId"] == "opencode-cli", "Model-only worker edits preserve the existing provider");
		payload = {{"workerBindings", {{"codex-cli", {{"workerProviderId", "opencode-cli"}}}}}};
		Require(PrepareSettingsDispatch("setMemorySettings", payload, state).Ok() && payload["workerBindings"]["codex-cli"]["workerModelId"] == "inferdeck/normal", "Omitted same-provider worker model is preserved");
		payload = {{"workerBindings", {{"codex-cli", {{"workerProviderId", "claude-cli"}}}}}};
		Require(PrepareSettingsDispatch("setMemorySettings", payload, state).Ok() && payload["workerBindings"]["codex-cli"]["workerModelId"] == "", "Changing worker provider clears an incompatible model as agreed");
		payload = {{"workerBindings", {{"gemini-cli", {{"workerModelId", "new-model"}}}}}};
		Require(PrepareSettingsDispatch("setMemorySettings", payload, state).status == 400, "A model edit cannot invent an unconfigured worker provider");
		payload = {{"goalMaxLoopIterations", 200}, {"acpTurnOutputLimitMiB", 1024}, {"acpSetupInactivityTimeoutSeconds", 600}, {"permissionReviewerProviderId", "opencode-cli"}, {"permissionReviewerModelId", "inferdeck/normal"}};
		const auto original = payload;
		Require(PrepareSettingsDispatch("setMemorySettings", payload, state).Ok() && payload == original, "Reviewer and runtime-limit changes must leave memory defaults untouched");
	}

	void SettingsPreferencesStorageAndAvailability()
	{
		const auto state = SettingsState();
		Json payload = {{"favoriteUamAgentIds", Json::array({"researcher"})}};
		Require(PrepareSettingsDispatch("setUamAgentPreferences", payload, state).Ok() && payload["uamAgentCycleShortcut"] == "alt+shift+tab", "Favorite edits preserve the desktop shortcut");
		payload = {{"uamAgentCycleShortcut", "disabled"}};
		Require(PrepareSettingsDispatch("setUamAgentPreferences", payload, state).Ok() && payload["favoriteUamAgentIds"] == Json::array({"reviewer"}), "Shortcut edits preserve favorites");
		payload = {{"worktreeRetentionDays", 30}};
		Require(PrepareSettingsDispatch("setVcsSettings", payload, state).Ok() && payload["gitEnabled"] == true && payload["svnEnabled"] == false && payload.size() == 3, "Retention edits supply current required VCS flags without resetting other settings");
		payload = {{"backupLogRetentionDays", 0}};
		Require(PrepareSettingsDispatch("setStorageSettings", payload, state).Ok() && !payload.contains("httpCacheClearIntervalDays"), "Omitted storage fields must reach the native post-Poll resolver without stale snapshot expansion");
		for (const char* id : uam::provider_ids::kAllCliProviderIds)
		{
			payload = {{"executionHostId", "remote"}, {"providerId", id}, {"enabled", false}};
			Require(PrepareSettingsDispatch("setProviderAvailability", payload, state).Ok(), "All six providers keep their explicit per-host availability switch");
		}
		payload["executionHostId"] = "unconfigured";
		Require(PrepareSettingsDispatch("setProviderAvailability", payload, state).status == 404, "Availability cannot create a new host");
	}

	void SettingsRejectPrivilegedAndMalformedUpdates()
	{
		for (const auto& request : std::vector<std::pair<std::string, Json>>{
		         {"setProviderMcpConfiguration", {{"executionHostId", "local"}, {"providerId", "codex-cli"}, {"serverId", "existing"}, {"enabled", true}, {"headers", {{"Authorization", "secret"}}}}},
		         {"setVcsSettings", {{"worktreeRetentionDays", 30}, {"gitEnabled", false}}},
		         {"setMemorySettings", {{"levelDefault", "strict"}, {"memoryDirectory", "/arbitrary"}}},
		         {"setStorageSettings", {{"backupLogRetentionDays", 0}, {"deleteChats", true}}},
		         {"getProviderMcpStatus", {{"executionHostId", "local"}, {"workspaceDirectory", "/arbitrary"}}}})
		{
			auto payload = request.second;
			Require(PrepareRequest(request.first, payload).status == 403, "Restricted settings cannot carry privileged replacement fields");
		}
		for (const auto& request : std::vector<std::pair<std::string, Json>>{
		         {"setProviderChatDefaults", {{"defaults", {{"unknown-provider", {{"modelId", "model"}}}}}}},
		         {"setProviderChatDefaults", {{"defaults", {{"codex-cli", {{"smallModelMode", "true"}}}}}}},
		         {"setProviderChatDefaults", {{"defaults", {{"codex-cli", {{"modelId", "--injected"}}}}}}},
		         {"setMemorySettings", {{"enabledDefault", false}, {"levelDefault", "open"}}},
		         {"setMemorySettings", {{"recallBudgetBytes", 8193}}},
		         {"setMemorySettings", {{"idleDelaySeconds", true}}},
		         {"setMemorySettings", {{"goalMaxLoopIterations", std::uint64_t(1) << 63}}},
		         {"setStorageSettings", {{"backupLogRetentionDays", 1.5}}},
		         {"setStorageSettings", {{"httpCacheClearIntervalDays", -1}}},
		         {"setUamAgentPreferences", {{"favoriteUamAgentIds", Json::array({"../agent"})}}},
		         {"setProviderMcpConfiguration", {{"executionHostId", "local"}, {"providerId", "codex-cli"}, {"serverId", "existing"}, {"enabled", "true"}}}})
		{
			auto payload = request.second;
			Require(PrepareRequest(request.first, payload).status == 400, "Malformed settings must fail before handler normalization can erase values");
		}
		for (const auto action : kWritableSettingsActions)
		{
			Json payload = Json::object();
			Require(PrepareRequest(action, payload).status == 400, "An empty settings patch cannot reset defaults");
		}
	}

	void SettingsMcpAndNestedProjectionRedaction()
	{
		auto state = SettingsState();
		auto& settings = state["settings"];
		settings["providerChatDefaults"]["codex-cli"]["unknownCredential"] = "secret";
		settings["hostScopedSettingsValues"]["remote"]["providerChatDefaults"]["codex-cli"]["environment"] = {{"KEY", "secret"}};
		settings["memoryWorkerBindings"]["codex-cli"]["credential"] = "secret";
		settings["memoryLastStatus"] = "raw secret model output";
		settings["centralProviderConfiguration"] = {{"token", "secret"}};
		settings["mcpServers"] = Json::array({{{"headers", {{"Authorization", "secret"}}}}});
		settings["worktreeRetentionDays"] = 30;
		const auto projected = MobileSettings(settings);
		Require(projected.dump().find("secret") == std::string::npos && projected["worktreeRetentionDays"] == 30 && projected["memoryWorkerBindings"]["codex-cli"]["workerModelId"] == "inferdeck/normal", "Nested defaults and worker projections retain display values without future secrets or raw diagnostics");
		Json raw = {{"executionHostId", "local"}, {"token", "secret"}, {"providers", Json::array({{
		    {"providerId", "codex-cli"}, {"status", "connected"}, {"detail", "raw secret provider output"}, {"uamControlEnabled", true},
		    {"mcpServers", Json::array({{{"id", "existing"}, {"name", "Tools"}, {"enabled", false}, {"command", "/secret"}, {"headers", {{"Authorization", "secret"}}}}})},
		    {"environment", {{"KEY", "secret"}}}
		}})}};
		const auto safe = MobileMcpStatus(raw);
		Require(safe.dump().find("secret") == std::string::npos && safe["providers"][0]["status"] == "connected" && safe["providers"][0]["mcpServers"][0]["enabled"] == false, "MCP status must retain real switch/connection state without raw provider text or configuration");
		Json payload = {{"executionHostId", "remote"}, {"providerId", "codex-cli"}, {"serverId", "existing"}, {"enabled", false}};
		Require(PrepareSettingsDispatch("setProviderMcpConfiguration", payload, state).Ok(), "Existing per-provider server toggle reaches native ownership validation");
		payload["executionHostId"] = "unknown";
		Require(PrepareSettingsDispatch("setProviderMcpConfiguration", payload, state).status == 404, "MCP toggles cannot invent execution hosts");
	}

	void SettingsTransferredEnvelopeUsesFinalPolicy()
	{
		for (const bool privileged : {false, true})
		{
			auto transfers = Transfers();
			Json payload = {{"levelDefault", "readOnly"}};
			if (privileged) payload["workerExecutable"] = "/arbitrary";
			const std::string body = Json{{"action", "setMemorySettings"}, {"payload", payload}}.dump();
			const auto transfer = transfers.Begin(body.size(), kNow);
			Require(transfers.Append(transfer.id, 0, uam::base64::Encode(body), kNow).Ok(), "Settings envelopes use existing authenticated transfer machinery");
			auto parsed = uam::cef::ParseBridgeRequest(transfers.Commit(transfer.id, kNow).body);
			Require(parsed.ok && PrepareRequest(parsed.request.action, parsed.request.payload).Ok() != privileged, "Uploaded settings have exactly the same restricted final policy as direct requests");
		}
	}

	void PolicyByteAttachmentStaging()
	{
		Json payload = {{"chatId", "chat-one"}, {"items", Json::array({{{"name", "photo.png"}, {"kind", "image"}, {"mimeType", "image/png"}, {"dataBase64", "YWI="}}})}};
		Require(PrepareRequest("stageChatAttachments", payload).Ok(), "Companion byte attachments must use the real staging action");
		for (const char* field : {"path", "previewOriginPath", "id", "sourcePath", "type"})
		{
			Json forged = payload;
			forged["items"][0][field] = "/private/host-secret";
			Require(PrepareRequest("stageChatAttachments", forged).status == 403, "Staging must never read a client-specified host path");
		}
		Json directory = payload;
		directory["items"][0]["kind"] = "directory";
		Require(PrepareRequest("stageChatAttachments", directory).status == 400, "Directory attachments cannot bypass the bytes-only boundary");
		Json traversal = payload;
		traversal["chatId"] = "../another-chat";
		Require(PrepareRequest("stageChatAttachments", traversal).status == 400, "Staging cannot use chat identity as a filesystem traversal");
		payload["items"][0]["dataBase64"] = "YWJ=";
		Require(PrepareRequest("stageChatAttachments", payload).status == 400, "Attachment bytes require strict base64");
		payload = {{"chatId", "chat-one"}, {"markdownStoreFiles", Json::array({"/private/host-secret"})}};
		Require(PrepareRequest("sendAcpPrompt", payload).status == 403, "Sending cannot attach arbitrary markdown store paths");
	}

	void AttachmentDescriptorOwnership()
	{
		StagedAttachments descriptors;
		const Json scope = {{"id", "chat-one"}, {"executionHostId", "local"}, {"workspaceDirectory", "/workspace"}};
		const Json issued = Json::array({{{"id", "server-attachment"}, {"path", ".UAM/attachments/chat-one/file.txt"}, {"copied", true}, {"previewOriginPath", "/workspace/.UAM/attachments/chat-one/file.txt"}}});
		Require(!descriptors.Allows("chat-one", issued, scope, kNow), "Client-invented descriptors must fail before staging");
		Require(descriptors.Remember("chat-one", issued, scope, kNow), "Server staging results must be retained");
		Require(descriptors.Allows("chat-one", issued, scope, kNow), "Exact staged result may be sent for the same chat");
		Require(!descriptors.Allows("chat-two", issued, scope, kNow), "Staged paths cannot cross chats");
		Json changed_scope = scope;
		changed_scope["executionHostId"] = "ssh-host";
		Require(!descriptors.Allows("chat-one", issued, changed_scope, kNow), "Staging cannot cross workspace/host changes");
		for (const char* field : {"path", "previewOriginPath", "id", "dataBase64"})
		{
			Json forged = issued;
			forged[0][field] = "/private/host-secret";
			Require(!descriptors.Allows("chat-one", forged, scope, kNow), "A caller cannot replace any server-issued attachment field");
		}
		Require(!descriptors.Allows("chat-one", issued, scope, kNow + std::chrono::hours(1)), "Expired staging results must be restaged explicitly");
	}

	std::vector<std::pair<std::string, Json>> ProviderActionRequests()
	{
		return {
		    {"listManagedSubagents", {{"chatId", "chat-one"}, {"offset", 0}, {"limit", 50}}},
		    {"getManagedAgentTranscript", {{"chatId", "chat-one"}, {"transcriptChatId", "child-one"}}},
		    {"cancelManagedSubagent", {{"chatId", "chat-one"}, {"runId", "550e8400-e29b-41d4-a716-446655440000"}, {"controlScopeId", "current-listing-scope"}}},
		    {"submitManagedSubagentFollowup", {{"chatId", "chat-one"}, {"runId", "550e8400-e29b-41d4-a716-446655440000"}, {"controlScopeId", "current-listing-scope"},
		        {"afterTurnId", "550e8400-e29b-41d4-a716-446655440001"}, {"clientRequestId", "550e8400-e29b-41d4-a716-446655440002"}, {"text", "Continue the owned read-only task."}}},
		    {"setNativeModelOption", {{"chatId", "chat-one"}, {"nativeSessionId", "native-one"}, {"executionHostId", "local"}, {"lifetime", "owned-process-lifetime"}, {"optionId", "autoRouting"}, {"value", "balanced"}}},
		    {"refreshNativeModelOptions", {{"chatId", "chat-one"}, {"nativeSessionId", "native-one"}, {"executionHostId", "local"}, {"lifetime", "owned-process-lifetime"}}},
		};
	}

	void PolicyProviderActionsCapabilitiesAndReadOnly()
	{
		Json state = {{"chats", Json::array({{{"id", "chat-one"}, {"importedReadOnly", false}}})}};
		const auto capabilities = Capabilities(state);
		for (const auto& [action, request] : ProviderActionRequests())
		{
			const bool read = action == "listManagedSubagents" || action == "getManagedAgentTranscript";
			const Action* policy = FindAction(action);
			Require(policy && policy->chat_scoped && policy->read_only == read, "Provider actions advertise their exact read/control boundary");
			Require(std::count_if(capabilities["actions"].begin(), capabilities["actions"].end(), [&](const Json& row) { return row["action"] == action && row["chatScoped"] == true && row["readOnly"] == read; }) == 1, "Every provider action is advertised exactly once");
			Json payload = request;
			Require(PrepareRequest(action, payload).Ok() && payload == request, "Valid provider requests retain their exact native identities without acquiring desktop selection");
			Require(AuthorizeChat(action, payload, state).Ok(), "Existing active chat requests pass the transport boundary before native ownership validation");
			state["chats"][0]["importedReadOnly"] = true;
			Require(AuthorizeChat(action, payload, state).Ok() == read, "Imported chats remain inspectable but cannot cancel, submit or operate native model controls");
			state["chats"][0]["importedReadOnly"] = false;
			if (!read)
			{
				payload["chatId"] = "missing";
				Require(AuthorizeChat(action, payload, state).status == 404, "Provider mutations cannot target an absent chat");
				Require(AuthorizeChat(action, request, Json::object()).status == 503, "Provider mutations fail closed without a current catalogue");
			}
		}
	}

	void PolicyProviderActionsRejectMalformedOrPrivilegedFields()
	{
		for (const auto& [action, request] : ProviderActionRequests())
		{
			for (const Json& bad_chat : {Json(), Json(7), Json(" ")})
			{
				Json payload = request; payload["chatId"] = bad_chat;
				Require(PrepareRequest(action, payload).status == 400, "Every provider action requires an explicit nonblank chat identity");
			}
			for (const char* forbidden : {"selectChat", "attachments", "command", "workspaceDirectory", "executionOverride"})
			{
				Json payload = request; payload[forbidden] = true;
				Require(PrepareRequest(action, payload).status == 400, "Provider actions cannot smuggle selection, media, command or execution authority");
			}
			for (auto field = request.begin(); field != request.end(); ++field)
			{
				if (field.key() == "chatId" || field.key() == "offset" || field.key() == "limit") continue;
				Json payload = request; payload.erase(field.key());
				Require(PrepareRequest(action, payload).status == 400, "Required provider ownership or choice fields cannot be omitted");
			}
		}
		Json listing = {{"chatId", "chat-one"}, {"runId", ""}, {"offset", 10000}, {"limit", 100}};
		Require(PrepareRequest("listManagedSubagents", listing).Ok(), "An optional empty run filter and bounded terminal page retain native listing semantics");
		for (const Json& invalid : {Json(-1), Json(10001), Json("0")})
		{
			Json payload = listing; payload["offset"] = invalid;
			Require(PrepareRequest("listManagedSubagents", payload).status == 400, "Managed page offsets retain their native bound");
		}
		for (const Json& invalid : {Json(0), Json(101), Json(-1)})
		{
			Json payload = listing; payload["limit"] = invalid;
			Require(PrepareRequest("listManagedSubagents", payload).status == 400, "Managed page limits retain their native bound");
		}
		Json defaults = ProviderActionRequests()[4].second; defaults["value"] = nullptr;
		Require(PrepareRequest("setNativeModelOption", defaults).Ok(), "An explicit native Default remains null rather than an invented string");
		defaults["value"] = false;
		Require(PrepareRequest("setNativeModelOption", defaults).status == 400, "Native model choices reject nonstring nonnull values");
		Json text = ProviderActionRequests()[3].second; text["text"] = std::string(32 * 1024 + 1, 'x');
		Require(PrepareRequest("submitManagedSubagentFollowup", text).status == 400, "Managed follow-up retains its native text budget");
		text["text"] = std::string("read\0write", 10);
		Require(PrepareRequest("submitManagedSubagentFollowup", text).status == 400, "NUL cannot hide a second instruction after visible text");
	}

	void PolicyProviderTransferredActionsUseFinalPolicy()
	{
		for (const auto& [action, request] : ProviderActionRequests())
		{
			for (bool malformed : {false, true})
			{
				Json payload = request;
				if (malformed) payload["selectChat"] = true;
				const std::string body = Json{{"action", action}, {"payload", payload}}.dump();
				auto transfers = Transfers();
				const auto begun = transfers.Begin(body.size(), kNow);
				Require(begun.Ok() && transfers.Append(begun.id, 0, uam::base64::Encode(body), kNow).Ok(), "A provider envelope traverses real request transfer without gaining authority");
				const auto committed = transfers.Commit(begun.id, kNow);
				auto parsed = uam::cef::ParseBridgeRequest(committed.body);
				Require(parsed.ok && PrepareRequest(parsed.request.action, parsed.request.payload).Ok() != malformed, "Direct and uploaded provider envelopes use identical final payload policy");
				Require(transfers.Commit(begun.id, kNow).status == 404, "Provider request uploads retain single-use commit behavior");
			}
		}
	}

	void TransferredEnvelopeFinalAllowlist()
	{
		for (const char* raw : {"{", "[]", "{\"action\":\"sendAcpPrompt\",\"payload\":[]}", "{\"action\":\"getCompanionToken\"}", "{\"action\":\"branchFromMessage\",\"payload\":{\"chatId\":\"chat-one\"}}"})
		{
			auto transfers = Transfers();
			const std::string body = raw;
			const auto transfer = transfers.Begin(body.size(), kNow);
			Require(transfers.Append(transfer.id, 0, uam::base64::Encode(body), kNow).Ok(), "Transport must accept envelope bytes without granting action authority");
			const auto committed = transfers.Commit(transfer.id, kNow);
			auto parsed = uam::cef::ParseBridgeRequest(committed.body);
			Require(!parsed.ok || !PrepareRequest(parsed.request.action, parsed.request.payload).Ok(), "Transferred JSON must use the same parser and final action allowlist");
			Require(transfers.Commit(transfer.id, kNow).status == 404, "Rejected envelopes must still consume the transfer");
		}
	}
} // namespace

int main(int argc, char** argv)
{
	// clang-format off
	const std::vector<std::pair<std::string, std::function<void()>>> tests = {
	    {"TransferRoundTripCommitOnce", TransferRoundTripCommitOnce},
	    {"InFlightRequestAdmissionSurvivesAsyncOwnership", InFlightRequestAdmissionSurvivesAsyncOwnership},
	    {"ResponseTransferAggregateBoundsAndFinalRelease", ResponseTransferAggregateBoundsAndFinalRelease},
	    {"ResponseTransferTimerExpiryAndIdentity", ResponseTransferTimerExpiryAndIdentity},
	    {"TransferMalformedOffsets", TransferMalformedOffsets},
	    {"TransferStrictBase64AndBounds", TransferStrictBase64AndBounds},
	    {"TransferOversizeCapacityAndRelease", TransferOversizeCapacityAndRelease},
	    {"TransferAbsoluteExpiry", TransferAbsoluteExpiry},
	    {"TransferIdCollision", TransferIdCollision},
	    {"TransferEnvelopeFieldValidation", TransferEnvelopeFieldValidation},
	    {"PolicyCapabilitiesAndRestrictions", PolicyCapabilitiesAndRestrictions},
	    {"PolicyScopeSelectionAndReadOnly", PolicyScopeSelectionAndReadOnly},
	    {"PolicyQueueStableIdentityAndHeldState", PolicyQueueStableIdentityAndHeldState},
	    {"PolicyQueueAdmissionTransitionAndStaleIdentity", PolicyQueueAdmissionTransitionAndStaleIdentity},
	    {"PolicyQueueRequiresBoundedIdentity", PolicyQueueRequiresBoundedIdentity},
	    {"PolicyQueueExistingEditAndReorderOwnership", PolicyQueueExistingEditAndReorderOwnership},
	    {"PolicySettingsRedaction", PolicySettingsRedaction},
	    {"SettingsProviderDefaultsPartialAndScoped", SettingsProviderDefaultsPartialAndScoped},
	    {"SettingsMemoryAndWorkerPartialUpdates", SettingsMemoryAndWorkerPartialUpdates},
	    {"SettingsPreferencesStorageAndAvailability", SettingsPreferencesStorageAndAvailability},
	    {"SettingsRejectPrivilegedAndMalformedUpdates", SettingsRejectPrivilegedAndMalformedUpdates},
	    {"SettingsMcpAndNestedProjectionRedaction", SettingsMcpAndNestedProjectionRedaction},
	    {"SettingsTransferredEnvelopeUsesFinalPolicy", SettingsTransferredEnvelopeUsesFinalPolicy},
	    {"PolicyByteAttachmentStaging", PolicyByteAttachmentStaging},
	    {"AttachmentDescriptorOwnership", AttachmentDescriptorOwnership},
	    {"PolicyProviderActionsCapabilitiesAndReadOnly", PolicyProviderActionsCapabilitiesAndReadOnly},
	    {"PolicyProviderActionsRejectMalformedOrPrivilegedFields", PolicyProviderActionsRejectMalformedOrPrivilegedFields},
	    {"PolicyProviderTransferredActionsUseFinalPolicy", PolicyProviderTransferredActionsUseFinalPolicy},
	    {"TransferredEnvelopeFinalAllowlist", TransferredEnvelopeFinalAllowlist},
	};
	// clang-format on
	int run = 0;
	for (const auto& [name, test] : tests)
	{
		if (argc > 1 && name != argv[1])
			continue;
		++run;
		try
		{
			test();
			std::cout << "PASS " << name << '\n';
		}
		catch (const std::exception& error)
		{
			std::cerr << "FAIL " << name << ": " << error.what() << '\n';
			return 1;
		}
	}
	return run == 0 ? 2 : 0;
}
