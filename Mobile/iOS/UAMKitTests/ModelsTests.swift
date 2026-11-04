import Foundation
import XCTest
@testable import UAMKit

final class ModelsTests: XCTestCase
{
    func testCatalogWorkspaceSpellingPreservesPlatformAndScopeBoundaries()
    {
        XCTAssertTrue(NativeModelCatalogWorkspace.matches("C:/Work/Project/", #"c:\work\project"#, platform: "windows"))
        XCTAssertTrue(NativeModelCatalogWorkspace.matches(#"C:\Work\Project"#, "c:/work/project"))
        XCTAssertTrue(NativeModelCatalogWorkspace.matches(#"\\server\share\Project"#, "//server/share/project/", platform: "windows"))
        XCTAssertTrue(NativeModelCatalogWorkspace.matches("/work/Project/", "/work/Project", platform: "linux"))
        XCTAssertFalse(NativeModelCatalogWorkspace.matches("/work/Project", "/work/project", platform: "linux"))
        XCTAssertFalse(NativeModelCatalogWorkspace.matches("/work/project", "/work/other", platform: "windows"))
        XCTAssertFalse(NativeModelCatalogWorkspace.matches(#"/work/a\b"#, "/work/a/b", platform: "macos"))
        XCTAssertFalse(NativeModelCatalogWorkspace.matches("~/project", "/home/synthetic/project", platform: "linux"))
        XCTAssertFalse(NativeModelCatalogWorkspace.matches("", "/"))
        XCTAssertTrue(NativeModelCatalogWorkspace.matches("/", "///"))
    }

    func testJSONRoundTripPreservesUnknownFieldsAndLargeIntegers() throws
    {
        let data = Data(#"{"provider":{"future":[null,true,18446744073709551615,-42,1.25]},"id":9007199254740993}"#.utf8)
        let value = try JSONDecoder().decode(JSONValue.self, from: data)
        XCTAssertEqual(value["id"], .integer(9007199254740993))
        XCTAssertEqual(value["provider"]?["future"]?[2], .unsignedInteger(UInt64.max))
        XCTAssertEqual(try JSONDecoder().decode(JSONValue.self, from: JSONEncoder().encode(value)), value)
        XCTAssertNil(JSONValue.bool(true).intValue)
        XCTAssertNil(JSONValue.number(1.5).intValue)
        XCTAssertNil(value[-1])
    }

    func testExactSummaryAndMessageFieldMapping() throws
    {
        let raw: JSONValue = [
            "id": "chat", "title": "Work", "providerId": "antigravity", "folderId": "folder",
            "createdAt": "2026-10-17", "updatedAt": "2026-10-17", "pinned": true,
            "messagesDigest": "opaque-v1", "messageCount": 3, "snoozedUntilMs": 1234,
            "acpSession": ["lifecycleState": "waitingUserInput", "providerExtra": ["x": true]],
            "cliTerminal": ["running": true], "futureField": ["opaque": 1]
        ]
        let chat = try ChatSummary(json: raw)
        XCTAssertEqual(chat.providerId, "antigravity")
        XCTAssertEqual(chat.messagesDigest, "opaque-v1")
        XCTAssertEqual(chat.snoozedUntilMs, 1234)
        XCTAssertEqual(chat.acpSession?["providerExtra"]?["x"], true)
        XCTAssertEqual(try JSONDecoder().decode(JSONValue.self, from: JSONEncoder().encode(chat)), raw)
        let message = try ChatMessage(json: ["role": "assistant", "content": "Hello", "createdAt": "now", "acpPromptNotSent": true, "blocks": [["type": "future", "payload": [1, 2]]]])
        XCTAssertEqual(message.content, "Hello")
        XCTAssertEqual(message.acpPromptNotSent, true)
        XCTAssertEqual(message.blocks?[0]["payload"], [1, 2])
    }

    func testRequiredFieldsAndPageBounds() throws
    {
        XCTAssertThrowsError(try ChatSummary(json: ["id": "incomplete"]))
        XCTAssertThrowsError(try ChatMessage(json: ["role": "assistant", "content": "missing timestamp"]))
        let page = try ChatMessagesPage(json: ["chatId": "chat", "startIndex": 10, "totalCount": 12, "messagesDigest": "revision", "userMessageIndices": .null, "messages": [message, message]])
        XCTAssertEqual(page.olderPageBefore, 10)
        XCTAssertEqual(page.messages?.count, 2)
        XCTAssertNil(page.userMessageIndices)
        XCTAssertThrowsError(try ChatMessagesPage(json: ["startIndex": 11, "totalCount": 12, "messages": [message, message]]))
        XCTAssertThrowsError(try ChatMessagesPage(json: ["startIndex": -1]))
        XCTAssertThrowsError(try ChatMessagesPage(json: ["totalCount": 2, "userMessageIndices": [2]]))
        let unchanged = try ChatMessagesPage(json: ["chatId": "chat", "unchanged": true, "messagesDigest": "revision"])
        XCTAssertNil(unchanged.messages)
        XCTAssertTrue(unchanged.unchanged)
        XCTAssertTrue(try InitialState(json: ["unchanged": true]).unchanged)
    }

    func testPagingRequestUsesExactBackendKeys() throws
    {
        let request = try ChatMessagesRequest(chatId: "chat", limit: 50, before: 100, messagesDigest: "revision")
        XCTAssertEqual(request.payload["before"], 100)
        XCTAssertEqual(request.payload["messagesDigest"], "revision")
        XCTAssertEqual(request.payload["deferToolCallContent"], true)
        XCTAssertThrowsError(try ChatMessagesRequest(chatId: "chat", limit: 201))
        XCTAssertThrowsError(try ChatMessagesRequest(chatId: "chat", before: -1))
    }

    private var message: JSONValue { ["role": "user", "content": "hi", "createdAt": "now"] }

    private var nativeAutoRouting: JSONValue
    {
        ["ownership": ["chatId": "chat", "nativeSessionId": "native", "executionHostId": "local", "lifetime": "public-owner", "privateNonce": "not-a-target"],
         "catalogStatus": "ready", "stateStatus": "ready", "eligibleTiers": ["present": true, "value": [
            ["id": " Vendor/Latency:Fast+V2 ", "displayName": "Low latency", "description": "Native choice", "type": "auto", "status": ["enabled": true]],
            ["id": "restricted", "displayName": "Restricted", "description": "", "type": "auto", "status": ["enabled": false, "message": "Disabled by provider policy"]],
            ["id": "named-model", "displayName": "Named", "description": "", "type": "model", "status": ["enabled": true]]]],
         "providerDefault": ["present": false], "currentModel": ["present": true, "value": "auto"],
         "effective": ["present": true, "value": " Vendor/Latency:Fast+V2 "],
         "pending": ["present": true, "value": .null], "activating": ["present": false, "value": "ignore-stale-value"],
         "requestPending": false, "available": true, "disabledCode": "", "disabledReason": "", "lastError": "", "lastAckStatus": "pending"]
    }

    func testNativeAutoRoutingRetainsOpaqueChoicesAndExplicitDefault() throws
    {
        let option = try CopilotAutoRoutingOption(json: nativeAutoRouting)
        XCTAssertEqual(option.pending, .value(nil), "A pending provider Default differs from no pending change")
        XCTAssertEqual(option.activating, .absent)
        XCTAssertEqual(option.name(for: " Vendor/Latency:Fast+V2 "), "Low latency")
        XCTAssertEqual(option.name(for: "Future/ExactCase"), "Future/ExactCase")
        let payload = try option.preferencePayload(" Vendor/Latency:Fast+V2 ")
        XCTAssertEqual(payload, ["chatId": "chat", "nativeSessionId": "native", "executionHostId": "local", "lifetime": "public-owner",
                                 "optionId": "autoRouting", "value": " Vendor/Latency:Fast+V2 "])
        XCTAssertEqual(try option.preferencePayload(nil)["value"], .null)
        XCTAssertEqual(option.ownership?.payload.objectValue?.count, 4, "Unknown owner fields never enter companion requests")
        XCTAssertThrowsError(try option.preferencePayload("vendor/latency:fast+v2"))
        XCTAssertThrowsError(try option.preferencePayload("restricted"))
        XCTAssertThrowsError(try option.preferencePayload("named-model"))
        XCTAssertEqual(option.tiers[1].disabledReason, "Disabled by provider policy")
    }

    func testNativeAutoRoutingRejectsMalformedAndOversizedProjections() throws
    {
        let first = nativeAutoRouting["eligibleTiers"]!["value"]![0]!
        var invalidTier = first; invalidTier["id"] = .string(String(repeating: "é", count: 257))
        var badBoolean = first; badBoolean["status"] = ["enabled": "true"]
        let malformed: [(String, JSONValue)] = [
            ("eligibleTiers", ["present": true, "value": [first, first]]),
            ("eligibleTiers", ["present": true, "value": .array(Array(repeating: first, count: 65))]),
            ("eligibleTiers", ["present": true, "value": .array([invalidTier])]),
            ("eligibleTiers", ["present": true, "value": .array([badBoolean])]),
            ("pending", ["present": true]), ("pending", ["present": true, "value": 0]),
            ("pending", ["present": "true", "value": .null]), ("effective", ["present": true, "value": .null]),
            ("available", "true"), ("catalogStatus", "future-status"), ("ownership", .null),
            ("ownership", ["chatId": "chat", "nativeSessionId": "native", "executionHostId": "local", "lifetime": "bad\0owner"])
        ]
        for (field, value) in malformed
        {
            var raw = nativeAutoRouting; raw[field] = value
            XCTAssertThrowsError(try CopilotAutoRoutingOption(json: raw), field)
        }
        var large = nativeAutoRouting
        large["eligibleTiers"] = ["present": true, "value": .array((0..<64).map { index in
            var tier = first; tier["id"] = .string("tier-\(index)"); tier["description"] = .string(String(repeating: "x", count: 2048)); return tier
        })]
        XCTAssertThrowsError(try CopilotAutoRoutingOption(json: large))
    }

    func testNativeAutoRoutingOwnershipAndReadinessAreSeparate() throws
    {
        let option = try CopilotAutoRoutingOption(json: nativeAutoRouting)
        func matches(chat: String = "chat", provider: String = "copilot-cli", runtimeProvider: String = "copilot-cli", protocolKind: String = "copilot-sdk",
                     session: String = "native", host: String = "local", readOnly: Bool = false) -> Bool
        {
            option.matches(chatID: chat, providerID: provider, runtimeProviderID: runtimeProvider, protocolKind: protocolKind,
                           nativeSessionID: session, executionHostID: host, readOnly: readOnly)
        }
        XCTAssertTrue(matches())
        XCTAssertFalse(matches(chat: "foreign")); XCTAssertFalse(matches(session: "foreign")); XCTAssertFalse(matches(host: "foreign"))
        XCTAssertFalse(matches(provider: "codex-cli")); XCTAssertFalse(matches(runtimeProvider: "claude-cli"))
        XCTAssertFalse(matches(protocolKind: "acp")); XCTAssertFalse(matches(readOnly: true))
        for (field, value) in [("requestPending", JSONValue.bool(true)), ("available", .bool(false)), ("stateStatus", .string("unknown")),
                               ("catalogStatus", .string("unsupported")), ("currentModel", ["present": true, "value": "Auto"]),
                               ("activating", ["present": true, "value": .null]), ("eligibleTiers", ["present": false])]
        {
            var raw = nativeAutoRouting; raw[field] = value
            XCTAssertThrowsError(try CopilotAutoRoutingOption(json: raw).preferencePayload(nil), field)
        }
        var ownerless = nativeAutoRouting; ownerless["ownership"] = nil
        let unavailable = try CopilotAutoRoutingOption(json: ownerless)
        XCTAssertNil(unavailable.ownership)
        XCTAssertThrowsError(try unavailable.preferencePayload(nil))
    }
}
