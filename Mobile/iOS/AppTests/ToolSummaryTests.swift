import XCTest
@testable import UAM

final class ToolSummaryTests: XCTestCase
{
    private func serialized(_ title: String, kind: String = "execute", content: String? = nil) -> WireObject
    {
        var tool: WireObject = ["id": "fixture-tool", "title": title, "kind": kind, "status": "completed",
                                "approvalStatus": "", "isSubAgent": false, "subAgentId": "", "subAgentTitle": "",
                                "contentDeferred": content == nil]
        if let content { tool["content"] = content }
        else { tool["contentDigest"] = "fixture-digest" }
        return tool
    }

    func testDeferredSerializerCommandTitleSurvivesEmptySubagentFields()
    {
        for kind in ["execute", "shell", "commandExecution"]
        {
            let summary = ToolSummary(serialized("  git status --short  ", kind: kind))
            XCTAssertEqual(summary.displayTitle, "git status --short")
            XCTAssertNil(summary.command)
        }
    }

    func testActualArgumentsResultEnvelopeProvidesCommandWithoutUsingResult()
    {
        let content = "Approval: allowed\n\nArguments:\n{\"command\":\"printf 'hello'\",\"cwd\":\"/fixture\"}\n\nResult:\n{\"command\":\"not-the-request\"}"
        let summary = ToolSummary(serialized(" \n ", content: content))
        XCTAssertEqual(summary.displayTitle, "printf 'hello'")
        XCTAssertEqual(summary.command, "printf 'hello'")
        let resultOnly = ToolSummary(serialized("git status", content: "{\"command\":\"not-the-request\"}"))
        XCTAssertEqual(resultOnly.displayTitle, "git status")
        XCTAssertNil(resultOnly.command)
    }

    func testOnlyPrefixedNestedJSONStringsHaveArgumentsProvenance() throws
    {
        let inner = "{\"command\":\"swift test --filter Fixture\"}"
        let wrapped = try JSONSerialization.data(withJSONObject: ["arguments": inner])
        let content = try XCTUnwrap(String(data: wrapped, encoding: .utf8))
        let ambiguous = ToolSummary(serialized("Execute", content: content))
        XCTAssertEqual(ambiguous.displayTitle, "Execute")
        XCTAssertNil(ambiguous.command)
        let prefixed = "Arguments:\n" + content + "\n\nResult:\nfixture output"
        XCTAssertEqual(ToolSummary(serialized("Execute", content: prefixed)).command, "swift test --filter Fixture")
        let nested = "Arguments:\n{\"rawInput\":{\"input\":{\"command\":\"pwd\"}}}\n\nResult:\n/fixture"
        XCTAssertEqual(ToolSummary(serialized("", content: nested)).displayTitle, "pwd")
    }

    func testReadWriteAndSearchTitlesAreRetainedWithoutCommandGuessing()
    {
        for (kind, title) in [("read", "Sources/App.swift"), ("edit", "Update Sources/App.swift"), ("search", "Find connection handlers")]
        {
            let summary = ToolSummary(serialized(title, kind: kind, content: "{\"command\":\"output-is-not-input\"}"))
            XCTAssertEqual(summary.displayTitle, title)
            XCTAssertNil(summary.command)
        }
        XCTAssertEqual(ToolSummary(serialized(" \n", kind: "read")).displayTitle, "Read file")
        XCTAssertEqual(ToolSummary(serialized("", kind: "write")).displayTitle, "Write file")
        XCTAssertEqual(ToolSummary(serialized("", kind: "search")).displayTitle, "Search")
    }

    func testChildTitleOnlyAppliesToActualSubagent()
    {
        var tool = serialized("Read configuration", kind: "read")
        tool["subAgentTitle"] = "Unrelated child title"
        XCTAssertEqual(ToolSummary(tool).displayTitle, "Read configuration")
        tool["isSubAgent"] = true
        XCTAssertEqual(ToolSummary(tool).displayTitle, "Unrelated child title")
        tool["subAgentTitle"] = " \n\t"
        XCTAssertEqual(ToolSummary(tool).displayTitle, "Read configuration")
        tool["title"] = ""
        XCTAssertEqual(ToolSummary(tool).displayTitle, "Subagent activity")
    }

    func testUnavailableMalformedOrResultOnlyContentHasNonemptyFallback()
    {
        for content in [nil, "", "Result:\n{\"command\":\"not-requested\"}", "Arguments:\n{incomplete", "{\"result\":{\"command\":\"not-requested\"}}"] as [String?]
        {
            let summary = ToolSummary(serialized(" \n", content: content))
            XCTAssertEqual(summary.displayTitle, "Run command")
            XCTAssertNil(summary.command)
        }
        XCTAssertEqual(ToolSummary([:]).displayTitle, "Tool activity")
        XCTAssertEqual(ToolSummary(serialized("", kind: "custom-provider-tool")).displayTitle, "custom-provider-tool")
    }

    func testAmbiguousRawJSONCannotTurnAnExecutionResultIntoACommand()
    {
        for title in ["", " \n", "Execute", "Command", "git status"]
        {
            for content in ["{\"command\":\"output-only\"}", "Approval: allowed\n\n{\"command\":\"args-or-output\"}"]
            {
                let summary = ToolSummary(serialized(title, content: content))
                let trimmed = title.trimmingCharacters(in: .whitespacesAndNewlines)
                XCTAssertEqual(summary.displayTitle, trimmed.isEmpty ? "Run command" : trimmed)
                XCTAssertNil(summary.command)
            }
        }
    }

    func testPreviewIsBoundedWithoutChangingOriginalToolDetails()
    {
        let original = String(repeating: "👩🏽‍💻", count: 300)
        let tool = serialized(original)
        let summary = ToolSummary(tool)
        XCTAssertEqual(summary.displayTitle.count, ToolSummary.maximumTitleCharacters + 1)
        XCTAssertTrue(summary.displayTitle.hasSuffix("…"))
        XCTAssertEqual(tool.text("title"), original)
        let huge = "Arguments:\n{\"command\":\"" + String(repeating: "x", count: 70_000) + "\"}\n\nResult:\nignored"
        XCTAssertEqual(ToolSummary(serialized("Command", content: huge)).displayTitle, "Command")
    }
}
