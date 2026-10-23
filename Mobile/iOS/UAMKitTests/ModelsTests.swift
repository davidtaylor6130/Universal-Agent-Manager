import Foundation
import XCTest
@testable import UAMKit

final class ModelsTests: XCTestCase
{
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
}
