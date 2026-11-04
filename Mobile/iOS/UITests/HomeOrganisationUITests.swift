import XCTest

/// Uses disposable --organisation (58966) and --expiry-check (58967) loopback fixtures.
@MainActor final class HomeOrganisationUITests: XCTestCase
{
    func testActiveSnoozedAndWorkingFamilyDestinations() throws
    {
        let app = launchFixture()
        let working = app.buttons["chat-org-root"]
        XCTAssertTrue(working.waitForExistence(timeout: 15))
        XCTAssertFalse(app.staticTexts.matching(NSPredicate(format: "label BEGINSWITH %@", "Settled")).firstMatch.exists)
        let snoozed = app.buttons["snoozed"]
        XCTAssertTrue(snoozed.exists)
        XCTAssertEqual(snoozed.label, "Snoozed Chats")
        XCTAssertTrue((snoozed.value as? String)?.contains("collapsed") == true)
        XCTAssertFalse(app.buttons["chat-org-snoozed"].exists)
        snoozed.tap()
        XCTAssertTrue(app.buttons["chat-org-snoozed"].waitForExistence(timeout: 5))
        capture(app, "Home-Active-and-Snoozed")

        working.tap()
        XCTAssertTrue(app.staticTexts["Working child conversation fixture."].waitForExistence(timeout: 10), "Active opens the actual working child.")
        XCTAssertFalse(app.staticTexts["Root conversation fixture."].exists)
        app.buttons["Back to chats"].tap()
        app.buttons["chat-org-snoozed"].tap()
        XCTAssertTrue(app.staticTexts["Snoozed conversation fixture."].waitForExistence(timeout: 10))
        app.buttons["Back to chats"].tap()

        let wake = app.buttons["wake-org-snoozed"]
        XCTAssertTrue(wake.waitForExistence(timeout: 5))
        wake.tap()
        let disappeared = XCTNSPredicateExpectation(predicate: NSPredicate(format: "exists == false"), object: app.buttons["snoozed"])
        XCTAssertEqual(XCTWaiter.wait(for: [disappeared], timeout: 10), .completed)
    }

    func testChatsPreservesFlatPinsOrderedCollectionsAndWorkspaceMembership() throws
    {
        let app = launchFixture()
        showChats(app)
        let pinned = app.buttons["pinned"]
        let all = app.buttons["all"]
        XCTAssertEqual(pinned.label, "Pinned Chats")
        XCTAssertEqual(all.label, "All Chats")
        XCTAssertLessThan(pinned.frame.minY, all.frame.minY)
        let pinnedRows = app.buttons.matching(identifier: "chat-org-pinned")
        XCTAssertEqual(pinnedRows.count, 1, "A pinned root remains visible while its workspace is collapsed.")
        XCTAssertLessThanOrEqual(pinnedRows.firstMatch.frame.maxY, all.frame.minY)
        let collection = app.buttons["collection-org-collection"]
        let workspace = app.buttons["workspace-org-workspace"]
        XCTAssertEqual(collection.label, "Work Collection")
        XCTAssertEqual(workspace.label, "Product Workspace")
        XCTAssertTrue((workspace.value as? String)?.contains("collapsed") == true)
        capture(app, "Home-Chats-Before-Collection-Interaction")
        let collectionOrder = app.buttons.allElementsBoundByIndex.map(\.identifier)
        XCTAssertLessThan(try XCTUnwrap(collectionOrder.firstIndex(of: "collection-org-collection")),
                          try XCTUnwrap(collectionOrder.firstIndex(of: "collection-org-empty-collection")),
                          "Keep the computer's collection order, not alphabetical order.")
        XCTAssertFalse(app.buttons["chat-org-root"].exists)
        capture(app, "Home-Chats-Flat-Pins-and-Collections")

        workspace.tap()
        let duplicatedPin = XCTNSPredicateExpectation(predicate: NSPredicate { _, _ in pinnedRows.count == 2 }, object: nil)
        XCTAssertEqual(XCTWaiter.wait(for: [duplicatedPin], timeout: 5), .completed, "The pinned root must also remain in All Chats.")
        XCTAssertTrue(app.buttons["chat-org-root"].exists)
        collection.tap()
        assertAbsent(app.buttons["workspace-org-workspace"])
        XCTAssertEqual(pinnedRows.count, 1)
        collection.tap()
        XCTAssertTrue(app.buttons["workspace-org-workspace"].waitForExistence(timeout: 5))
        XCTAssertTrue((app.buttons["workspace-org-workspace"].value as? String)?.contains("expanded") == true)

        let emptyWorkspace = app.buttons["workspace-org-empty-workspace"]
        reveal(emptyWorkspace, in: app)
        XCTAssertTrue(app.staticTexts["No chats in this workspace"].exists)
        let emptyCollection = app.buttons["collection-org-empty-collection"]
        reveal(emptyCollection, in: app)
        XCTAssertTrue((emptyCollection.value as? String)?.contains("collapsed") == true)
        emptyCollection.tap()
        XCTAssertTrue(app.staticTexts["No workspaces in this collection"].waitForExistence(timeout: 5))
        let personalWorkspace = app.buttons["workspace-org-ungrouped"]
        reveal(personalWorkspace, in: app)
        XCTAssertEqual(personalWorkspace.label, "Personal Workspace")
        let openCode = app.buttons["chat-org-opencode"]
        reveal(openCode, in: app)
        XCTAssertTrue(openCode.label.contains("OpenCode"))
        capture(app, "Home-Chats-Collections-and-OpenCode")
        openCode.tap()
        XCTAssertTrue(app.staticTexts["OpenCode conversation fixture."].waitForExistence(timeout: 10))
        capture(app, "Home-OpenCode-Conversation")
        app.buttons["Back to chats"].tap()
        let unsorted = app.buttons["unsorted"]
        reveal(unsorted, in: app)
        XCTAssertEqual(unsorted.label, "Unsorted")
        let unfiled = app.buttons["chat-org-unsorted"]
        reveal(unfiled, in: app)
        unfiled.tap()
        XCTAssertTrue(app.staticTexts["Unsorted conversation fixture."].waitForExistence(timeout: 10))
    }

    func testDescendantSearchRevealsAncestorsAndOpensTheBrowseRoot() throws
    {
        let app = launchFixture()
        showChats(app)
        app.buttons["collection-org-collection"].tap()
        assertAbsent(app.buttons["workspace-org-workspace"])
        app.buttons["Search chats"].tap()
        let search = app.textFields["chatSearch"]
        XCTAssertTrue(search.waitForExistence(timeout: 5))
        search.tap(); search.typeText("   ")
        assertAbsent(app.buttons["workspace-org-workspace"], "A whitespace-only query must preserve the collapsed collection.")
        let clear = app.buttons["Clear search"]
        XCTAssertTrue(clear.waitForExistence(timeout: 5), "Raw whitespace input remains explicitly clearable.")
        clear.tap()
        search.tap(); search.typeText("Needle")
        let root = app.buttons["chat-org-root"]
        XCTAssertTrue(root.waitForExistence(timeout: 5))
        XCTAssertTrue(app.buttons["collection-org-collection"].exists)
        XCTAssertTrue(app.buttons["workspace-org-workspace"].exists)
        let child = app.buttons["chat-org-child"]
        XCTAssertTrue(child.waitForExistence(timeout: 5), "Search retains the real root and reveals its matching child directly in All Chats.")
        XCTAssertTrue(child.label.contains("Branch of"), "A direct branch search result explains its source chat.")
        capture(app, "Home-Descendant-Search-and-Ancestors")
        child.tap()
        XCTAssertTrue(app.staticTexts["Working child conversation fixture."].waitForExistence(timeout: 10))
        XCTAssertFalse(app.staticTexts["Root conversation fixture."].exists)
        app.buttons["Back to chats"].tap()
        XCTAssertTrue(root.waitForExistence(timeout: 5))
        root.tap()
        XCTAssertTrue(app.staticTexts["Root conversation fixture."].waitForExistence(timeout: 10), "The retained browse root remains independently selectable.")
        XCTAssertFalse(app.staticTexts["Working child conversation fixture."].exists)
        app.buttons["Back to chats"].tap()
        app.buttons["Close search"].tap()
        assertAbsent(app.buttons["workspace-org-workspace"], "Search must not overwrite the user's collapsed collection.")
    }

    func testSnoozedDeadlineExpiresWithoutAnotherSuccessfulSnapshot() throws
    {
        let app = launchFixture(port: 58967)
        let snoozed = app.buttons["snoozed"]
        XCTAssertTrue(snoozed.waitForExistence(timeout: 5))
        snoozed.tap()
        let parked = app.buttons["chat-org-snoozed"]
        XCTAssertTrue(parked.waitForExistence(timeout: 5))
        capture(app, "Home-Snooze-Deadline-Before-Expiry")
        let interrupted = app.descendants(matching: .any).matching(identifier: "connectionStatus").firstMatch
        XCTAssertTrue(interrupted.waitForExistence(timeout: 8), "Subsequent snapshot requests deliberately fail while the last catalogue stays visible.")
        let expired = XCTNSPredicateExpectation(predicate: NSPredicate(format: "exists == false"), object: snoozed)
        XCTAssertEqual(XCTWaiter.wait(for: [expired], timeout: 20), .completed, "The local deadline must remove expired snooze without a fresh snapshot.")
        XCTAssertFalse(app.buttons["wake-org-snoozed"].exists, "The expired chat is no longer parked, even if it now appears in Active.")
        XCTAssertTrue(parked.exists, "Snooze expiry returns the unsettled chat to Active without deleting it.")
        XCTAssertTrue(app.buttons["chat-org-root"].exists, "A failed refresh preserves the accepted catalogue.")
        capture(app, "Home-Snooze-Deadline-Expired-While-Disconnected")
    }

    func testLightLargeTypeHierarchyAndOpenCodeNavigation() throws
    {
        let app = launchFixture(arguments: ["-uam.appearance", "light", "-UIPreferredContentSizeCategoryName", "UICTContentSizeCategoryAccessibilityXXXL"])
        showChats(app)
        let pinned = app.buttons["pinned"]
        let all = app.buttons["all"]
        XCTAssertEqual(pinned.label, "Pinned Chats")
        XCTAssertEqual(all.label, "All Chats")
        XCTAssertTrue(app.buttons["chat-org-pinned"].exists, "Pinned chats remain flat while their workspace is collapsed.")
        XCTAssertGreaterThanOrEqual(pinned.frame.height, 44)
        reveal(all, in: app)
        XCTAssertGreaterThanOrEqual(all.frame.height, 44)
        let collection = app.buttons["collection-org-collection"]
        reveal(collection, in: app)
        XCTAssertGreaterThanOrEqual(collection.frame.height, 44)
        let workspace = app.buttons["workspace-org-workspace"]
        reveal(workspace, in: app)
        XCTAssertGreaterThanOrEqual(workspace.frame.height, 44)
        XCTAssertTrue((workspace.value as? String)?.contains("collapsed") == true)
        capture(app, "Home-Light-Large-Type-Hierarchy")
        let personal = app.buttons["workspace-org-ungrouped"]
        reveal(personal, in: app)
        let openCode = app.buttons["chat-org-opencode"]
        reveal(openCode, in: app)
        XCTAssertGreaterThanOrEqual(openCode.frame.height, 44)
        XCTAssertTrue(openCode.label.contains("OpenCode"))
        capture(app, "Home-Light-Large-Type-OpenCode")
        openCode.tap()
        XCTAssertTrue(app.staticTexts["OpenCode conversation fixture."].waitForExistence(timeout: 10))
        capture(app, "Home-Light-Large-Type-OpenCode-Conversation")
    }

    private func launchFixture(port: Int = 58966, arguments: [String] = []) -> XCUIApplication
    {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchArguments = arguments
        app.launchEnvironment["UAM_TEST_ENDPOINT"] = "http://127.0.0.1:\(port)"
        app.launchEnvironment["UAM_TEST_TOKEN"] = "uam-isolated-ui-test-token"
        app.launchEnvironment["UAM_TEST_DRAFT_NAMESPACE"] = "organisation-" + UUID().uuidString
        app.launch()
        XCTAssertTrue(app.buttons["chat-org-root"].waitForExistence(timeout: 15), "Start the isolated fixture for port \(port) before these tests.")
        return app
    }

    private func showChats(_ app: XCUIApplication)
    {
        app.buttons["Chats"].tap()
        XCTAssertTrue(app.buttons["all"].waitForExistence(timeout: 5))
    }

    private func assertAbsent(_ element: XCUIElement, _ message: String = "A collapsed section hides its rows.", file: StaticString = #filePath, line: UInt = #line)
    {
        let disappeared = XCTNSPredicateExpectation(predicate: NSPredicate(format: "exists == false"), object: element)
        XCTAssertEqual(XCTWaiter.wait(for: [disappeared], timeout: 5), .completed, message, file: file, line: line)
    }

    private func reveal(_ element: XCUIElement, in app: XCUIApplication, file: StaticString = #filePath, line: UInt = #line)
    {
        for _ in 0..<7
        {
            if element.exists && element.isHittable { return }
            app.scrollViews["homeChatList"].swipeUp()
        }
        XCTAssertTrue(element.exists && element.isHittable, "Expected organisation row must be reachable.", file: file, line: line)
    }

    private func capture(_ app: XCUIApplication, _ name: String)
    {
        let attachment = XCTAttachment(screenshot: app.screenshot())
        attachment.name = name; attachment.lifetime = .keepAlways; add(attachment)
    }
}
