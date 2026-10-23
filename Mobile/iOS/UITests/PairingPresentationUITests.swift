import XCTest

/// Exercise real sheet presentation and subsequent touches, not just AX hit areas.
@MainActor final class PairingPresentationUITests: XCTestCase
{
    override func tearDown() async throws
    {
        let attachment = XCTAttachment(screenshot: XCUIApplication().screenshot())
        attachment.name = name
        attachment.lifetime = .keepAlways
        add(attachment)
        let hierarchy = XCTAttachment(string: XCUIApplication().debugDescription)
        hierarchy.name = "Presentation hierarchy"
        hierarchy.lifetime = .keepAlways
        add(hierarchy)
    }

    func testIncomingLinkWhileNestedFailureKeepsCloseAndSettingsUsable() throws
    {
        let app = launchFixture()
        openNestedPairing(app)
        let address = app.textFields["pairAddress"]
        address.tap(); address.typeText("https://127.0.0.1:1")
        let token = app.secureTextFields["pairToken"]
        token.tap(); token.typeText("disposable-presentation-fixture")
        app.buttons["connectEnvironment"].tap()
        XCTAssertTrue(app.staticTexts["pairFailure"].waitForExistence(timeout: 15))
        let passwordPrompt = app.sheets["Save Password?"]
        if passwordPrompt.waitForExistence(timeout: 3) { passwordPrompt.buttons["Not Now"].tap() }
        XCUIDevice.shared.system.open(try failureLink())
        XCTAssertTrue(app.staticTexts["pairIgnoredLink"].waitForExistence(timeout: 5))
        XCTAssertEqual(app.buttons.matching(identifier: "pairClose").count, 1)
        app.buttons["pairClose"].tap()
        assertAbsent(app.buttons["pairClose"])
        assertSettingsNavigationWorks(app)
    }

    func testIncomingLinkWhileNestedSuccessKeepsCloseAndSettingsUsable() throws
    {
        guard let certificate = ProcessInfo.processInfo.environment["UAM_TEST_PAIRING_CERTIFICATE"] else
        { throw XCTSkip("Start the isolated HTTPS pairing fixture and supply its public root.") }
        let app = launchFixture()
        openNestedPairing(app)
        var parts = URLComponents()
        parts.scheme = "uam"; parts.host = "pair"
        parts.queryItems = [URLQueryItem(name: "url", value: "https://127.0.0.1:58963/companion"),
                            URLQueryItem(name: "token", value: "uam-isolated-ui-test-token"),
                            URLQueryItem(name: "name", value: "Presentation fixture computer"),
                            URLQueryItem(name: "certificate", value: certificate)]
        XCUIDevice.shared.system.open(try XCTUnwrap(parts.url))
        XCTAssertTrue(app.staticTexts["pairSuccess"].waitForExistence(timeout: 20))
        XCUIDevice.shared.system.open(try failureLink())
        XCTAssertTrue(app.staticTexts["pairIgnoredLink"].waitForExistence(timeout: 5))
        XCTAssertTrue(app.staticTexts["pairSuccess"].exists)
        XCTAssertEqual(app.buttons.matching(identifier: "pairClose").count, 1)
        app.buttons["pairClose"].tap()
        assertAbsent(app.buttons["pairClose"])
        assertSettingsNavigationWorks(app)
    }

    func testIncomingLinkWaitsUntilChooseProjectDismisses() throws
    {
        let app = launchFixture()
        app.buttons["newChat"].tap()
        XCTAssertTrue(app.navigationBars["Choose project"].waitForExistence(timeout: 5))
        XCUIDevice.shared.system.open(try failureLink())
        XCTAssertTrue(app.navigationBars["Choose project"].exists)
        XCTAssertFalse(app.staticTexts["pairFailure"].exists, "An incoming link must not present from underneath the current sheet.")
        app.navigationBars.buttons["Cancel"].tap()
        XCTAssertTrue(app.staticTexts["pairFailure"].waitForExistence(timeout: 15), "The queued link opens only after the original sheet finishes dismissing.")
        app.buttons["pairClose"].tap()
        assertAbsent(app.buttons["pairClose"])
        app.buttons["newChat"].tap()
        XCTAssertTrue(app.navigationBars["Choose project"].waitForExistence(timeout: 5))
        app.navigationBars.buttons["Cancel"].tap()
        app.buttons["settings"].tap()
        XCTAssertTrue(app.buttons["settingsEnvironments"].waitForExistence(timeout: 5))
        app.buttons["Done"].tap()
        XCTAssertTrue(app.buttons["settings"].waitForExistence(timeout: 5))
    }

    private func launchFixture() -> XCUIApplication
    {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchEnvironment["UAM_TEST_ENDPOINT"] = "http://127.0.0.1:58962"
        app.launchEnvironment["UAM_TEST_TOKEN"] = "uam-isolated-ui-test-token"
        app.launchEnvironment["UAM_TEST_DRAFT_NAMESPACE"] = UUID().uuidString
        app.launch()
        XCTAssertTrue(app.buttons["settings"].waitForExistence(timeout: 15))
        return app
    }

    private func openNestedPairing(_ app: XCUIApplication)
    {
        app.buttons["settings"].tap()
        app.buttons["settingsEnvironments"].tap()
        app.buttons["Add environment"].tap()
        XCTAssertTrue(app.textFields["pairAddress"].waitForExistence(timeout: 5))
    }

    private func failureLink() throws -> URL
    {
        var parts = URLComponents()
        parts.scheme = "uam"; parts.host = "pair"
        parts.queryItems = [URLQueryItem(name: "url", value: "https://127.0.0.1:1"),
                            URLQueryItem(name: "token", value: "disposable-presentation-fixture"),
                            URLQueryItem(name: "name", value: "Presentation fixture computer")]
        return try XCTUnwrap(parts.url)
    }

    private func assertAbsent(_ element: XCUIElement, file: StaticString = #filePath, line: UInt = #line)
    {
        let disappeared = XCTNSPredicateExpectation(predicate: NSPredicate(format: "exists == false"), object: element)
        XCTAssertEqual(XCTWaiter.wait(for: [disappeared], timeout: 5), .completed, file: file, line: line)
    }

    private func assertSettingsNavigationWorks(_ app: XCUIApplication)
    {
        XCTAssertTrue(app.buttons["Add environment"].waitForExistence(timeout: 5))
        app.buttons["Add environment"].tap()
        XCTAssertTrue(app.textFields["pairAddress"].waitForExistence(timeout: 5))
        app.navigationBars.buttons["Cancel"].tap()
        assertAbsent(app.textFields["pairAddress"])
        app.navigationBars["Environments"].buttons["Settings"].tap()
        app.buttons["Done"].tap()
        XCTAssertTrue(app.buttons["settings"].waitForExistence(timeout: 5))
        app.buttons["settings"].tap()
        XCTAssertTrue(app.buttons["settingsEnvironments"].waitForExistence(timeout: 5), "A new touch must navigate after closing the pairing result.")
        app.buttons["Done"].tap()
    }
}
