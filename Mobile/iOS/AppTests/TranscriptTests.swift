import XCTest
@testable import UAM

final class TranscriptTests: XCTestCase
{
    func testTextToolsAndFinalAnswerRetainOrderWithoutDuplicatingContent()
    {
        let message = MobileMessage(index: 7, raw: ["role": "assistant", "content": "BeforeAfter", "toolCalls": [["id": "one"], ["id": "two"]],
            "blocks": [["type": "assistant_text", "text": "Before"], ["type": "tool_call", "toolCallId": "one"], ["type": "tool_call", "toolCallId": "two"], ["type": "assistant_text", "text": "After"]]])
        let segments = TranscriptSegments.build(message)
        XCTAssertEqual(segments.map(\.kind), [.text, .tools, .text])
        XCTAssertEqual(segments.map(\.text), ["Before", "", "After"])
        XCTAssertEqual(segments[1].toolIDs, ["one", "two"])
    }

    func testLegacyContentAndUnreferencedToolsRemainVisible()
    {
        let message = MobileMessage(index: 0, raw: ["role": "assistant", "content": "Kept", "toolCalls": [["id": "orphan"]]])
        let segments = TranscriptSegments.build(message)
        XCTAssertEqual(segments.map(\.kind), [.text, .tools])
        XCTAssertEqual(segments.first?.text, "Kept")
        XCTAssertEqual(segments.last?.toolIDs, ["orphan"])
    }
}
