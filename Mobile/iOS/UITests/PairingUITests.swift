import XCTest

@MainActor final class PairingUITests: XCTestCase
{
    func testPrivateCAPairingShowsCheckingAndPersistentSuccess() throws
    {
        guard let certificate = ProcessInfo.processInfo.environment["UAM_TEST_PAIRING_CERTIFICATE"] else
        { throw XCTSkip("Start the isolated HTTPS pairing fixture and supply its public root.") }
        continueAfterFailure = false
        let springboard = XCUIApplication(bundleIdentifier: "com.apple.springboard")
        if springboard.alerts.firstMatch.buttons["Cancel"].exists { springboard.alerts.firstMatch.buttons["Cancel"].tap() }
        let app = XCUIApplication()
        app.launchEnvironment["UAM_TEST_ENDPOINT"] = "http://127.0.0.1:58962"
        app.launchEnvironment["UAM_TEST_TOKEN"] = "uam-isolated-ui-test-token"
        app.launchEnvironment["UAM_TEST_DRAFT_NAMESPACE"] = UUID().uuidString
        app.launch()
        XCTAssertTrue(app.buttons["settings"].waitForExistence(timeout: 15))
        var link = URLComponents()
        link.scheme = "uam"; link.host = "pair"
        link.queryItems = [URLQueryItem(name: "url", value: "https://127.0.0.1:58963/companion"),
                           URLQueryItem(name: "token", value: "uam-isolated-ui-test-token"),
                           URLQueryItem(name: "name", value: "Secure test computer"),
                           URLQueryItem(name: "certificate", value: certificate)]
        app.open(try XCTUnwrap(link.url))
        XCTAssertTrue(app.staticTexts["pairChecking"].waitForExistence(timeout: 5))
        let checking = XCTAttachment(screenshot: app.screenshot())
        checking.name = "Pairing-Secure-Checking"; checking.lifetime = .keepAlways; add(checking)
        XCTAssertTrue(app.staticTexts["pairSuccess"].waitForExistence(timeout: 20))
        XCTAssertTrue(app.buttons["pairClose"].isHittable)
        XCTAssertFalse(app.textFields["pairAddress"].exists)
        let success = XCTAttachment(screenshot: app.screenshot())
        success.name = "Pairing-Secure-Connected"; success.lifetime = .keepAlways; add(success)
        XCTAssertTrue(app.staticTexts["pairSuccess"].exists, "The result must remain until explicitly closed")
        app.buttons["pairClose"].tap()
    }

    func testManualFailureHasCloseAndRetainsDetailsForEditing() throws
    {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchEnvironment["UAM_TEST_ENDPOINT"] = "http://127.0.0.1:58962"
        app.launchEnvironment["UAM_TEST_TOKEN"] = "uam-isolated-ui-test-token"
        app.launchEnvironment["UAM_TEST_DRAFT_NAMESPACE"] = UUID().uuidString
        app.launch()
        XCTAssertTrue(app.buttons["settings"].waitForExistence(timeout: 15))
        app.buttons["settings"].tap()
        app.buttons["settingsEnvironments"].tap()
        app.buttons["Add environment"].tap()
        let address = app.textFields["pairAddress"]
        XCTAssertTrue(address.waitForExistence(timeout: 5))
        address.tap(); address.typeText("https://127.0.0.1:1")
        let token = app.secureTextFields["pairToken"]
        token.tap(); token.typeText("disposable-pairing-ui-fixture")
        let check = app.buttons["connectEnvironment"]
        XCTAssertTrue(check.isHittable, "The primary action must remain visible above the keyboard")
        check.tap()
        XCTAssertTrue(app.staticTexts["pairFailure"].waitForExistence(timeout: 15))
        // iOS may offer to save manually entered credentials in its own window.
        // Decline only that explicit system prompt before testing app controls.
        let passwordPrompt = app.sheets["Save Password?"]
        if passwordPrompt.waitForExistence(timeout: 3) { passwordPrompt.buttons["Not Now"].tap() }
        XCTAssertFalse(app.textFields["pairAddress"].exists, "Failure must replace the entry form")
        let close = app.buttons["pairClose"]
        let edit = app.buttons["pairEditDetails"]
        let settled = XCTNSPredicateExpectation(predicate: NSPredicate(format: "isHittable == true"), object: close)
        let closeIsHittable = XCTWaiter.wait(for: [settled], timeout: 10) == .completed
        let editIsHittable = edit.isHittable
        let hierarchy = XCTAttachment(string: app.debugDescription)
        hierarchy.name = "Pairing-Failure-Hierarchy"; hierarchy.lifetime = .keepAlways; add(hierarchy)
        XCTAssertFalse(app.staticTexts.matching(NSPredicate(format: "label CONTAINS %@", "Check the chat")).firstMatch.exists)
        let capture = XCTAttachment(screenshot: app.screenshot())
        capture.name = "Pairing-Explicit-Failure"; capture.lifetime = .keepAlways; add(capture)
        edit.tap()
        XCTAssertTrue(address.waitForExistence(timeout: 5), "The visible Edit details touch target must open the retained form")
        XCTAssertTrue(closeIsHittable, "Close must expose an accessible touch target after the failure layout settles")
        XCTAssertTrue(editIsHittable, "Edit details must expose an accessible touch target")
        XCTAssertEqual(address.value as? String, "https://127.0.0.1:1")
        XCTAssertTrue(app.buttons["connectEnvironment"].isEnabled, "The token must remain available for an explicit retry")
        app.buttons["Cancel"].tap()
    }
}
