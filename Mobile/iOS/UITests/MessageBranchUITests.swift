import XCTest

/// Disposable saved history only; --message-branches on loopback 58968 never runs a provider.
@MainActor final class MessageBranchUITests: XCTestCase
{
    private let computerName = "Main Universal Agent Manager computer with a long descriptive name"

    func testPinnedPromptAlternativesNavigateAtTheirUserMessageAndKeepDraft() throws
    {
        let app = launchFixture()
        openRoot(app)
        XCTAssertFalse(app.buttons["relatedChats"].exists)
        let previous = app.buttons["previous-message-branch-0"]
        let next = app.buttons["next-message-branch-0"]
        reveal(next, in: app)
        assertTouchSize(previous)
        assertTouchSize(next)
        assertCounter(app, index: 0, current: 1, total: 3)
        XCTAssertFalse(previous.isEnabled)
        XCTAssertTrue(next.isEnabled)
        capture(app, "Message-Branches-Original-User-Prompt")

        let draft = "Keep this local branch draft " + UUID().uuidString
        let input = app.textViews["composerInput"]
        input.tap(); input.typeText(draft)
        app.buttons["Back to chats"].tap()
        app.buttons.matching(identifier: "chat-mb-root").firstMatch.tap()
        XCTAssertTrue(input.waitForExistence(timeout: 10))
        XCTAssertEqual(input.value as? String, draft)
        reveal(next, in: app)
        next.tap()
        assertCounter(app, index: 0, current: 2, total: 3)
        assertResponse("First branch response.", in: app)
        reveal(next, in: app)
        next.tap()
        assertCounter(app, index: 0, current: 3, total: 3)
        reveal(previous, in: app)
        assertTouchSize(previous)
        assertTouchSize(next)
        XCTAssertTrue(previous.isHittable, "An older originating prompt remains reachable when the selected branch has more than one history page.")
        assertResponse("Second branch response.", in: app)
        capture(app, "Message-Branches-Long-History-Original-Prompt")
        XCTAssertFalse(next.isEnabled)
        reveal(previous, in: app)
        previous.tap()
        assertCounter(app, index: 0, current: 2, total: 3)
        reveal(previous, in: app)
        previous.tap()
        assertCounter(app, index: 0, current: 1, total: 3)
        XCTAssertEqual(input.value as? String, draft, "Returning to the original prompt path restores its exact unsent draft.")

        reveal(next, in: app)
        next.tap()
        let deeper = app.buttons["next-message-branch-2"]
        reveal(deeper, in: app, later: true)
        assertCounter(app, index: 2, current: 1, total: 2)
        deeper.tap()
        assertCounter(app, index: 2, current: 2, total: 2)
        assertResponse("Deeper branch response.", in: app)
        capture(app, "Message-Branches-Deeper-User-Prompt")
        let back = app.buttons["previous-message-branch-2"]
        reveal(back, in: app)
        back.tap()
        assertCounter(app, index: 2, current: 1, total: 2)
        assertResponse("First branch follow-up.", in: app)
    }

    func testExecuteToolsHaveCommandsAndRetainFullDeferredOutput() throws
    {
        let app = launchFixture()
        openRoot(app)
        let status = app.buttons["openTool-mb-status"]
        if !status.exists
        {
            let group = app.buttons.matching(NSPredicate(format: "label CONTAINS %@", "3 tool calls")).firstMatch
            reveal(group, in: app, later: true)
            group.tap()
        }
        reveal(status, in: app, later: true)
        XCTAssertTrue(status.label.contains("git status --short"), "A blank saved title must not reduce an execute tool to an icon.")
        let diff = app.buttons["openTool-mb-diff"]
        reveal(diff, in: app, later: true)
        XCTAssertTrue(diff.label.contains("git diff --stat"))
        capture(app, "Message-Tools-Readable-Command-Summaries")
        reveal(status, in: app)
        status.tap()
        XCTAssertTrue(app.staticTexts.matching(NSPredicate(format: "label CONTAINS %@", "fixture working tree clean")).firstMatch.waitForExistence(timeout: 5))
        app.buttons["Done"].tap()
        let deferred = app.buttons["openTool-mb-deferred"]
        reveal(deferred, in: app, later: true)
        XCTAssertGreaterThan(deferred.label.trimmingCharacters(in: .whitespacesAndNewlines).count, "Open tool:".count)
        deferred.tap()
        let output = app.staticTexts.matching(NSPredicate(format: "label CONTAINS %@", "fixture full deferred output marker")).firstMatch
        XCTAssertTrue(output.waitForExistence(timeout: 10))
        XCTAssertTrue(output.label.contains("git log -1 --oneline"), "Full original arguments remain available with the output.")
        XCTAssertTrue(app.navigationBars["git log -1 --oneline"].waitForExistence(timeout: 5), "Loaded original command arguments update the tool sheet title.")
        capture(app, "Message-Tools-Full-Deferred-Output")
        app.buttons["Done"].tap()
    }

    func testComputerSelectorKeepsLongNameInMenuAndVoiceOverValue() throws
    {
        let app = launchFixture()
        let computer = app.buttons["computerFilter"]
        XCTAssertTrue(computer.waitForExistence(timeout: 5))
        XCTAssertEqual(computer.label, "Choose computer")
        XCTAssertEqual(computer.value as? String, computerName)
        XCTAssertTrue(computer.staticTexts["Computer"].exists)
        XCTAssertFalse(app.staticTexts[computerName].exists)
        XCTAssertFalse(app.staticTexts["Test Mac"].exists)
        computer.tap()
        let selected = app.buttons.matching(NSPredicate(format: "label CONTAINS %@", computerName)).firstMatch
        XCTAssertTrue(selected.waitForExistence(timeout: 5))
        capture(app, "Computer-Selector-Full-Name-and-Selection")
        selected.tap()
        XCTAssertTrue(app.buttons["computerFilter"].waitForExistence(timeout: 5))
    }

    func testComputerSelectorUsesCompactIconAtAccessibilitySize() throws
    {
        let app = launchFixture(arguments: ["-UIPreferredContentSizeCategoryName", "UICTContentSizeCategoryAccessibilityXXXL"])
        let computer = app.buttons["computerFilter"]
        XCTAssertTrue(computer.waitForExistence(timeout: 5))
        XCTAssertEqual(computer.label, "Choose computer")
        XCTAssertEqual(computer.value as? String, computerName)
        XCTAssertFalse(computer.staticTexts["Computer"].exists, "Accessibility sizes use the labelled laptop icon without a wide visual text pill.")
        assertTouchSize(computer)
        let projects = app.buttons["projectFilter"]
        XCTAssertTrue(projects.exists && projects.isHittable)
        XCTAssertEqual(projects.label, "Choose project")
        XCTAssertEqual(projects.value as? String, "All projects")
        XCTAssertFalse(projects.staticTexts["All projects"].exists, "Large text uses a compact folder icon with the full value available to VoiceOver.")
        assertTouchSize(projects)
        XCTAssertGreaterThanOrEqual(projects.frame.minX, app.frame.minX)
        XCTAssertLessThanOrEqual(projects.frame.maxX, app.frame.maxX)
        XCTAssertGreaterThanOrEqual(projects.frame.minY, app.frame.minY)
        XCTAssertLessThanOrEqual(projects.frame.maxY, app.frame.maxY)
        capture(app, "Computer-Selector-Accessibility-Size")
    }

    func testCancelledBranchPagingKeepsConversationControlsUsable() throws
    {
        let app = launchFixture()
        openRoot(app)
        let next = app.buttons["next-message-branch-0"]
        reveal(next, in: app)
        next.tap()
        assertCounter(app, index: 0, current: 2, total: 3)
        reveal(next, in: app)
        next.tap()
        let cancel = app.buttons["cancelBranchOpening"]
        XCTAssertTrue(cancel.waitForExistence(timeout: 5), "Run the branch fixture with --message-branch-delay 3 for this cancellation check.")
        XCTAssertTrue(app.buttons["Chat actions"].isEnabled)
        let copy = app.buttons.matching(NSPredicate(format: "label == %@", "Copy message")).firstMatch
        XCTAssertTrue(copy.exists && copy.isEnabled, "Loading older branch history must not disable unrelated transcript controls.")
        XCTAssertFalse(app.buttons["relatedChats"].exists)
        cancel.tap()
        let stopped = XCTNSPredicateExpectation(predicate: NSPredicate(format: "exists == false"), object: cancel)
        XCTAssertEqual(XCTWaiter.wait(for: [stopped], timeout: 5), .completed)
        app.buttons["Chat actions"].tap()
        XCTAssertTrue(app.buttons["Rename"].waitForExistence(timeout: 5))
        app.buttons["Rename"].tap()
        XCTAssertTrue(app.alerts["Rename chat"].waitForExistence(timeout: 5))
        app.alerts["Rename chat"].buttons["Cancel"].tap()
        app.buttons["Back to chats"].tap()
        XCTAssertTrue(app.buttons["pinned"].waitForExistence(timeout: 5), "The next ordinary navigation remains responsive after cancellation.")
        capture(app, "Message-Branches-Cancelled-Paging-Returned-Home")
    }

    private func launchFixture(arguments: [String] = []) -> XCUIApplication
    {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchArguments = arguments
        app.launchEnvironment["UAM_TEST_ENDPOINT"] = "http://127.0.0.1:58968"
        app.launchEnvironment["UAM_TEST_TOKEN"] = "uam-isolated-ui-test-token"
        app.launchEnvironment["UAM_TEST_CONNECTION_NAME"] = computerName
        app.launchEnvironment["UAM_TEST_DRAFT_NAMESPACE"] = "message-branches-" + UUID().uuidString
        app.launch()
        XCTAssertTrue(app.buttons["settings"].waitForExistence(timeout: 15))
        app.buttons["Chats"].tap()
        XCTAssertTrue(app.buttons["pinned"].waitForExistence(timeout: 10), "Start fixture_server.py --message-branches first.")
        XCTAssertFalse(app.buttons["branches-pinned-mb-root"].exists, "Pinned rows never add a family disclosure.")
        return app
    }

    private func openRoot(_ app: XCUIApplication)
    {
        app.buttons.matching(identifier: "chat-mb-root").firstMatch.tap()
        XCTAssertTrue(app.textViews["composerInput"].waitForExistence(timeout: 10))
    }

    private func assertTouchSize(_ element: XCUIElement, file: StaticString = #filePath, line: UInt = #line)
    {
        // AX's CGRect conversion can represent44pt as43.99999999999994.
        // Only floating-point roundoff is tolerated; the44pt requirement is unchanged.
        let epsilon: CGFloat = 0.000001
        XCTAssertGreaterThanOrEqual(element.frame.width + epsilon, 44, file: file, line: line)
        XCTAssertGreaterThanOrEqual(element.frame.height + epsilon, 44, file: file, line: line)
    }

    private func assertResponse(_ text: String, in app: XCUIApplication, file: StaticString = #filePath, line: UInt = #line)
    {
        let response = app.staticTexts[text]
        XCTAssertTrue(response.waitForExistence(timeout: 10), file: file, line: line)
        reveal(response, in: app, later: true, file: file, line: line)
    }

    private func assertCounter(_ app: XCUIApplication, index: Int, current: Int, total: Int, file: StaticString = #filePath, line: UInt = #line)
    {
        let group = app.descendants(matching: .any).matching(identifier: "message-branch-\(index)").firstMatch
        let count = "\(current)/\(total)"
        let counter = XCTNSPredicateExpectation(predicate: NSPredicate { _, _ in
            guard group.exists else { return false }
            return (group.value as? String) == count || group.label.contains(count)
                || group.staticTexts.matching(NSPredicate(format: "label == %@ OR label == %@", count, "\(current) / \(total)")).firstMatch.exists
        }, object: nil)
        XCTAssertEqual(XCTWaiter.wait(for: [counter], timeout: 10), .completed, "Expected prompt branch position \(count).", file: file, line: line)
    }

    private func reveal(_ element: XCUIElement, in app: XCUIApplication, later: Bool = false, file: StaticString = #filePath, line: UInt = #line)
    {
        for _ in 0..<5
        {
            if element.exists && element.isHittable { return }
            if later { app.scrollViews.firstMatch.swipeUp() }
            else { app.scrollViews.firstMatch.swipeDown() }
        }
        XCTAssertTrue(element.exists && element.isHittable, "Expected transcript control must be reachable.", file: file, line: line)
    }

    private func capture(_ app: XCUIApplication, _ name: String)
    {
        let image = XCTAttachment(screenshot: app.screenshot())
        image.name = name; image.lifetime = .keepAlways; add(image)
    }
}
