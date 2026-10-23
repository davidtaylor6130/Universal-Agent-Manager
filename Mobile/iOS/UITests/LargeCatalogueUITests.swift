import XCTest

@MainActor final class LargeCatalogueUITests: XCTestCase
{
    override func tearDown() async throws
    {
        let attachment = XCTAttachment(screenshot: XCUIApplication().screenshot())
        attachment.name = name
        attachment.lifetime = .keepAlways
        add(attachment)
    }

    func testActiveSearchKeepsFamilyWhenOnlyChildMatches() throws
    {
        let app = launchStandardFixture()
        XCTAssertTrue(app.buttons["chat-mobile-root"].waitForExistence(timeout: 10))
        app.buttons["Search chats"].tap()
        let search = app.textFields["chatSearch"]
        XCTAssertTrue(search.waitForExistence(timeout: 5))
        search.typeText("Attachment review")
        XCTAssertTrue(app.buttons["chat-mobile-root"].waitForExistence(timeout: 5), "Matching an active child must retain its family root and activity destination.")
        app.buttons["chat-mobile-root"].tap()
        XCTAssertTrue(app.textViews["composerInput"].waitForExistence(timeout: 5))
        XCTAssertTrue(app.staticTexts["Attachment review"].exists)
    }

    func testLargeCatalogueBrowseAndBroadSearchRemainResponsive() throws
    {
        let app = XCUIApplication()
        app.launchEnvironment["UAM_TEST_ENDPOINT"] = "http://127.0.0.1:58965"
        app.launchEnvironment["UAM_TEST_TOKEN"] = "uam-isolated-ui-test-token"
        app.launchEnvironment["UAM_TEST_DRAFT_NAMESPACE"] = UUID().uuidString
        app.launch()
        XCTAssertTrue(app.buttons["Chats"].waitForExistence(timeout: 15))
        app.buttons["Chats"].tap()
        let more = app.buttons["more-mobile-project"]
        XCTAssertTrue(more.waitForExistence(timeout: 5))
        more.tap()
        XCTAssertTrue(app.buttons["Search chats"].isHittable)
        app.buttons["Search chats"].tap()
        let search = app.textFields["chatSearch"]
        XCTAssertTrue(search.waitForExistence(timeout: 5))
        search.typeText("a")
        XCTAssertTrue(app.buttons["chat-mobile-pinned"].firstMatch.waitForExistence(timeout: 5))
        let close = app.buttons["Close search"]
        XCTAssertTrue(close.isHittable)
        close.tap()
        XCTAssertTrue(app.buttons["settings"].waitForExistence(timeout: 5))
        app.buttons["settings"].tap()
        XCTAssertTrue(app.buttons["settingsEnvironments"].waitForExistence(timeout: 5))
    }

    func testReadAuthenticationFailureHasUsablePairingRecovery() throws
    {
        let app = launchStandardFixture(token: "wrong-disposable-fixture-token")
        XCTAssertTrue(app.staticTexts["connectionStatus"].waitForExistence(timeout: 10))
        let rejected = app.staticTexts.matching(NSPredicate(format: "label CONTAINS %@", "did not accept the saved access token")).firstMatch
        XCTAssertTrue(rejected.waitForExistence(timeout: 5))
        XCTAssertTrue(app.staticTexts["Connection unavailable"].exists)
        XCTAssertFalse(app.staticTexts["All quiet"].exists, "An unconnected catalogue cannot report that all chats are quiet.")
        let retry = app.buttons["retryConnection"]
        XCTAssertGreaterThanOrEqual(retry.frame.height, 44)
        let pair = app.buttons["pairConnectionAgain"]
        XCTAssertTrue(pair.exists)
        XCTAssertGreaterThanOrEqual(pair.frame.height, 44)
        pair.tap()
        XCTAssertTrue(app.textFields["pairAddress"].waitForExistence(timeout: 5))
        app.navigationBars.buttons["Cancel"].tap()
        XCTAssertTrue(app.buttons["settings"].waitForExistence(timeout: 5))
    }

    private func launchStandardFixture(token: String = "uam-isolated-ui-test-token") -> XCUIApplication
    {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchEnvironment["UAM_TEST_ENDPOINT"] = "http://127.0.0.1:58962"
        app.launchEnvironment["UAM_TEST_TOKEN"] = token
        app.launchEnvironment["UAM_TEST_DRAFT_NAMESPACE"] = UUID().uuidString
        app.launch()
        return app
    }

    func testLargeCatalogueKeepsChatNavigationResponsive() throws
    {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchEnvironment["UAM_TEST_ENDPOINT"] = "http://127.0.0.1:58965"
        app.launchEnvironment["UAM_TEST_TOKEN"] = "uam-isolated-ui-test-token"
        app.launchEnvironment["UAM_TEST_DRAFT_NAMESPACE"] = UUID().uuidString
        let started = Date()
        app.launch()
        let row = app.buttons["chat-mobile-root"]
        XCTAssertTrue(row.waitForExistence(timeout: 10))
        XCTAssertLessThan(Date().timeIntervalSince(started), 15, "A large catalogue must not block startup with repeated date parsing.")
        XCTAssertFalse(app.buttons["chat-quiet-4"].exists, "Settled chats must stay out of Active.")
        for _ in 0..<3
        {
            let opening = Date()
            row.tap()
            XCTAssertTrue(app.textViews["composerInput"].waitForExistence(timeout: 5))
            XCTAssertLessThan(Date().timeIntervalSince(opening), 8, "Opening a chat must keep navigation responsive.")
            let closing = Date()
            app.buttons["Back to chats"].tap()
            XCTAssertTrue(app.buttons["settings"].waitForExistence(timeout: 5))
            XCTAssertTrue(row.exists)
            XCTAssertLessThan(Date().timeIntervalSince(closing), 8, "Returning home must not reparse every date inside sort comparisons.")
        }
        app.buttons["settings"].tap()
        XCTAssertTrue(app.buttons["settingsEnvironments"].waitForExistence(timeout: 5), "The next touch must still work after repeated chat navigation.")
        app.buttons["Done"].tap()
        let attachment = XCTAttachment(screenshot: app.screenshot())
        attachment.name = "Large catalogue after repeated chat navigation"
        attachment.lifetime = .keepAlways
        add(attachment)
    }
}
