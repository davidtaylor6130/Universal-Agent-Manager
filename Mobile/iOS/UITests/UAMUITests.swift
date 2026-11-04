import XCTest

@MainActor final class UAMUITests: XCTestCase
{
    override func setUp() async throws
    {
        continueAfterFailure = false
    }
    override func tearDown() async throws
    {
        let screenshot = XCTAttachment(screenshot: XCUIApplication().screenshot())
        screenshot.name = name
        screenshot.lifetime = .keepAlways
        add(screenshot)
    }
    private func launchConnected(arguments: [String] = []) -> XCUIApplication
    {
        let app = XCUIApplication()
        app.launchEnvironment["UAM_TEST_ENDPOINT"] = "http://127.0.0.1:58962"
        app.launchEnvironment["UAM_TEST_TOKEN"] = "uam-isolated-ui-test-token"
        app.launchEnvironment["UAM_TEST_DRAFT_NAMESPACE"] = UUID().uuidString
        app.launchArguments = arguments
        app.launch()
        return app
    }

    func testHomeConversationAndDraftPersistence() throws
    {
        let app = launchConnected()
        let chat = app.buttons["chat-mobile-root"]
        XCTAssertTrue(chat.waitForExistence(timeout: 20))
        chat.tap()
        let input = app.textViews["composerInput"]
        XCTAssertTrue(input.waitForExistence(timeout: 10))
        XCTAssertLessThan(input.frame.height, 130, "The composer must not expand into the transcript.")
        input.tap()
        input.typeText("A retained mobile draft")
        app.buttons["Back to chats"].tap()
        chat.tap()
        XCTAssertTrue(input.waitForExistence(timeout: 10))
        XCTAssertEqual(input.value as? String, "A retained mobile draft")
        XCTAssertFalse(app.staticTexts.matching(NSPredicate(format: "label CONTAINS %@", "Request cancelled")).firstMatch.exists)
        app.terminate()
        let relaunched = XCUIApplication()
        relaunched.launchEnvironment = app.launchEnvironment
        relaunched.launch()
        let reopenedChat = relaunched.buttons["chat-mobile-root"]
        XCTAssertTrue(reopenedChat.waitForExistence(timeout: 20))
        XCTAssertTrue(reopenedChat.isHittable)
        reopenedChat.tap()
        let reopenedInput = relaunched.textViews["composerInput"]
        XCTAssertTrue(reopenedInput.waitForExistence(timeout: 10))
        XCTAssertEqual(reopenedInput.value as? String, "A retained mobile draft")
    }

    func testHomeSearchFocusAndFiltering() throws
    {
        let app = launchConnected()
        XCTAssertTrue(app.buttons["chat-mobile-root"].waitForExistence(timeout: 20))
        app.buttons["Search chats"].tap()
        let search = app.textFields["chatSearch"]
        XCTAssertTrue(search.waitForExistence(timeout: 5))
        XCTAssertTrue(app.keyboards.firstMatch.waitForExistence(timeout: 5), "Search should focus the keyboard immediately.")
        search.typeText("unmatched-native-search-fixture")
        XCTAssertTrue(app.staticTexts["No matching active chats"].waitForExistence(timeout: 5))
        app.buttons["Close search"].tap()
        XCTAssertTrue(app.buttons["chat-mobile-root"].waitForExistence(timeout: 5))
    }

    func testSettingsEnvironmentAndUsageNavigation() throws
    {
        let app = launchConnected()
        XCTAssertTrue(app.buttons["settings"].waitForExistence(timeout: 15))
        app.buttons["settings"].tap()
        capture(app, "Settings-Root")
        app.buttons["settingsEnvironments"].tap()
        XCTAssertTrue(app.staticTexts["Test Mac"].waitForExistence(timeout: 10))
        capture(app, "Saved-Environments")
        app.buttons["Add environment"].tap()
        XCTAssertTrue(app.textFields["pairAddress"].waitForExistence(timeout: 5))
        XCTAssertTrue(app.secureTextFields["pairToken"].exists)
        XCTAssertFalse(app.buttons["connectEnvironment"].isEnabled)
        capture(app, "Add-Environment-Manual")
        app.buttons["Cancel"].tap()
        app.navigationBars.buttons.element(boundBy: 0).tap()
        app.buttons["settingsLimits"].tap()
        XCTAssertTrue(app.staticTexts["Weekly"].waitForExistence(timeout: 10))
        XCTAssertTrue(app.staticTexts["42%"].exists)
        capture(app, "Usage-Limits")
        app.navigationBars.buttons.element(boundBy: 0).tap()
        app.buttons["Usage history"].tap()
        XCTAssertTrue(app.staticTexts["Providers"].waitForExistence(timeout: 10))
        capture(app, "Usage-History")
    }

    func testNewChatDraftSurvivesDismissalAndBackground() throws
    {
        let app = launchConnected()
        let marker = "retained-new-chat-draft-" + UUID().uuidString.lowercased()
        XCTAssertTrue(app.buttons["newChat"].waitForExistence(timeout: 20))
        app.buttons["newChat"].tap()
        XCTAssertTrue(app.buttons["project-mobile-project"].waitForExistence(timeout: 10))
        app.buttons["project-mobile-project"].tap()
        let input = app.textViews["composerInput"]
        XCTAssertTrue(input.waitForExistence(timeout: 10))
        input.tap(); input.typeText(marker)

        XCUIDevice.shared.press(.home)
        app.activate()
        XCTAssertTrue(input.waitForExistence(timeout: 10))
        XCTAssertEqual(input.value as? String, marker)
        XCTAssertTrue(app.navigationBars["New chat"].exists, "Foregrounding must keep the draft in the new-chat composer without sending it.")
        XCTAssertFalse(app.buttons["Back to chats"].exists)
        capture(app, "New-Chat-Draft-After-Background")

        app.navigationBars.buttons["Cancel"].tap()
        XCTAssertTrue(app.buttons["newChat"].waitForExistence(timeout: 10))
        app.buttons["newChat"].tap()
        XCTAssertTrue(app.buttons["project-mobile-project"].waitForExistence(timeout: 10))
        app.buttons["project-mobile-project"].tap()
        XCTAssertTrue(input.waitForExistence(timeout: 10))
        XCTAssertEqual(input.value as? String, marker, "Dismissal must synchronously flush the project draft before the debounce can be cancelled.")
        XCTAssertTrue(app.navigationBars["New chat"].exists)
        XCTAssertFalse(app.buttons["Back to chats"].exists)
        XCTAssertFalse(app.buttons["Dismiss error"].exists)
        capture(app, "New-Chat-Draft-Reopened")
        app.navigationBars.buttons["Cancel"].tap()
        XCTAssertTrue(app.buttons["newChat"].waitForExistence(timeout: 10), "Finish back on home without creating or sending a chat.")
    }

    func testProjectCreationAndRealSend() throws
    {
        let app = launchConnected()
        XCTAssertTrue(app.buttons["newChat"].waitForExistence(timeout: 15))
        app.buttons["newChat"].tap()
        XCTAssertTrue(app.buttons["project-mobile-project"].waitForExistence(timeout: 10))
        app.buttons["project-mobile-project"].tap()
        let input = app.textViews["composerInput"]
        XCTAssertTrue(input.waitForExistence(timeout: 10))
        input.tap(); input.typeText("Verify native mobile send")
        app.buttons["sendMessage"].tap()
        XCTAssertTrue(app.staticTexts["The isolated host received your message."].waitForExistence(timeout: 15))
        XCTAssertLessThan(input.frame.height, 40, "Empty composer should collapse after sending.")
    }

    func testReferenceScreensAndChildNavigation() throws
    {
        let app = launchConnected()
        XCTAssertTrue(app.buttons["chat-mobile-root"].waitForExistence(timeout: 20))
        capture(app, "Home-Active")
        app.buttons["Chats"].tap()
        XCTAssertTrue(app.buttons["pinned"].waitForExistence(timeout: 5))
        capture(app, "Home-Chats")
        app.buttons["chat-mobile-root"].tap()
        XCTAssertTrue(app.textViews["composerInput"].waitForExistence(timeout: 10))
        XCTAssertTrue(app.staticTexts["Native mobile experience"].exists, "Chats must open the chosen conversation, rather than its active child.")
        capture(app, "Conversation-Collapsed")
        app.textViews["composerInput"].tap()
        app.textViews["composerInput"].typeText("Review this change before we continue.")
        capture(app, "Conversation-Keyboard")
        app.buttons["Add attachment"].tap()
        XCTAssertTrue(app.buttons["Photo library"].waitForExistence(timeout: 5))
        capture(app, "Composer-Attachments")
    }

    func testChildNavigationAndStopPreserveTypedDraft() throws
    {
        let app = launchConnected()
        XCTAssertTrue(app.buttons["Chats"].waitForExistence(timeout: 20))
        app.buttons["Chats"].tap()
        app.buttons["chat-mobile-root"].tap()
        let child = app.buttons["openSubagent-tool-child"]
        XCTAssertTrue(child.waitForExistence(timeout: 10))
        child.tap()
        XCTAssertTrue(app.staticTexts.matching(NSPredicate(format: "label CONTAINS %@", "Original files remain on the main computer.")).firstMatch.waitForExistence(timeout: 10))
        let input = app.textViews["composerInput"]
        input.tap(); input.typeText("Retain this draft when stopping the child")
        app.buttons["Chat actions"].tap()
        XCTAssertTrue(app.buttons["Stop response"].waitForExistence(timeout: 5), "Stop must remain accessible while a draft is being typed.")
        app.buttons["Stop response"].tap()
        XCTAssertEqual(input.value as? String, "Retain this draft when stopping the child")
        capture(app, "Child-Conversation-Draft-Retained")
        XCTAssertFalse(app.buttons["relatedChats"].exists)
        app.buttons["Chat actions"].tap()
        XCTAssertTrue(app.buttons["returnToParentChat"].waitForExistence(timeout: 5))
        app.buttons["returnToParentChat"].tap()
        XCTAssertTrue(app.staticTexts["Native mobile experience"].waitForExistence(timeout: 10))
        XCTAssertEqual(input.value as? String ?? "", "", "The child's draft must not move into the parent.")
    }

    func testCodeOverflowCanReachEndAndFittingTableHasNoCue() throws
    {
        let app = launchConnected()
        XCTAssertTrue(app.buttons["chat-mobile-root"].waitForExistence(timeout: 20))
        app.buttons["chat-mobile-root"].tap()
        let code = app.scrollViews["markdownCodeScroll"].firstMatch
        XCTAssertTrue(code.waitForExistence(timeout: 10))
        let hint = app.staticTexts["markdownCodeScroll-overflowHint"]
        XCTAssertTrue(hint.waitForExistence(timeout: 5))
        let content = app.staticTexts["markdownCodeContent"].firstMatch
        let original = content.label
        XCTAssertTrue(original.hasSuffix("token: token)"))
        for _ in 0..<5
        {
            if code.value as? String == "Horizontal position 100 percent" { break }
            code.swipeLeft()
        }
        XCTAssertEqual(code.value as? String, "Horizontal position 100 percent")
        XCTAssertEqual(content.label, original, "Panning must expose the original code without changing it.")
        capture(app, "Code-Scrolled-To-End")
        app.buttons["openSubagent-tool-child"].tap()
        let table = app.scrollViews["markdownTableScroll"].firstMatch
        XCTAssertTrue(table.waitForExistence(timeout: 10))
        XCTAssertFalse(app.staticTexts["markdownTableScroll-overflowHint"].exists, "A fitting table should not claim hidden content.")
        capture(app, "Fitting-Table")
    }

    func testDefaultModelPreservesLowercaseIdentifier() throws
    {
        let app = launchConnected()
        XCTAssertTrue(app.buttons["settings"].waitForExistence(timeout: 15))
        app.buttons["settings"].tap()
        let defaults = app.buttons["Chat defaults"]
        if !defaults.isHittable { app.swipeUp() }
        defaults.tap()
        XCTAssertTrue(app.buttons["providerDefaults-opencode-cli"].waitForExistence(timeout: 10))
        app.buttons["providerDefaults-opencode-cli"].tap()
        let field = app.textFields["defaultModelID"]
        XCTAssertTrue(field.waitForExistence(timeout: 10))
        field.tap()
        if let existing = field.value as? String, existing != "Provider default"
        { field.typeText(String(repeating: XCUIKeyboardKey.delete.rawValue, count: existing.count)) }
        field.typeText("normal")
        app.buttons["saveMainSettings"].tap()
        XCTAssertTrue(app.staticTexts["Saved on your computer"].waitForExistence(timeout: 15))
        XCTAssertEqual(field.value as? String, "normal")
        capture(app, "Settings-Exact-Model")
    }

    func testLandscapeConversationKeepsComposerAndNavigationUsable() throws
    {
        XCUIDevice.shared.orientation = .portrait
        let app = launchConnected()
        XCTAssertTrue(app.buttons["chat-mobile-root"].waitForExistence(timeout: 20))
        app.buttons["chat-mobile-root"].tap()
        XCTAssertTrue(app.textViews["composerInput"].waitForExistence(timeout: 10))
        XCUIDevice.shared.orientation = .landscapeLeft
        defer { XCUIDevice.shared.orientation = .portrait }
        let landscape = NSPredicate { _, _ in app.frame.width > app.frame.height }
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(predicate: landscape, object: nil)], timeout: 10), .completed)
        let window = app.windows.firstMatch
        let input = app.textViews["composerInput"]
        let back = app.buttons["Back to chats"]
        let microphone = app.buttons["dictateMessage"]
        let send = app.buttons["sendMessage"]
        for control in [back, microphone, send] { XCTAssertTrue(control.waitForExistence(timeout: 5)) }
        var lastFrames: [CGRect] = []
        var stableSamples = 0
        let stable = NSPredicate { _, _ in
            let frames = [window.frame, input.frame, back.frame, microphone.frame, send.frame]
            stableSamples = frames == lastFrames ? stableSamples + 1 : 0
            lastFrames = frames
            return stableSamples >= 3 && window.frame.width > window.frame.height
        }
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(predicate: stable, object: nil)], timeout: 10), .completed)
        XCTAssertLessThanOrEqual(input.frame.width, 720, "Landscape text entry should retain a readable column.")
        XCTAssertGreaterThan(back.frame.minX, window.frame.minX + 40, "The wide conversation header should be centred with the reading column.")
        XCTAssertTrue(back.isHittable)
        XCTAssertTrue(input.isHittable)
        XCTAssertTrue(microphone.isHittable)
        let bounds = window.frame.insetBy(dx: -1, dy: -1)
        for (name, frame) in [("composer", input.frame), ("back", back.frame), ("microphone", microphone.frame), ("send", send.frame)]
        {
            XCTAssertGreaterThan(frame.width, 0, name)
            XCTAssertGreaterThan(frame.height, 0, name)
            XCTAssertTrue(bounds.contains(frame), "\(name) \(frame) must fit within app window \(window.frame)")
        }
        let appShot = app.screenshot()
        let windowShot = window.screenshot()
        let screenShot = XCUIScreen.main.screenshot()
        func attach(_ shot: XCUIScreenshot, _ name: String)
        {
            let attachment = XCTAttachment(screenshot: shot)
            attachment.name = name; attachment.lifetime = .keepAlways; add(attachment)
        }
        attach(appShot, "Landscape-App-Capture")
        attach(windowShot, "Landscape-Window-Capture")
        attach(screenShot, "Landscape-Full-Screen-Capture")
        let image = screenShot.image
        let upright = UIGraphicsImageRenderer(size: image.size).image { _ in image.draw(at: .zero) }
        let normalized = XCTAttachment(image: upright)
        normalized.name = "Landscape-Full-Screen-Normalized"; normalized.lifetime = .keepAlways; add(normalized)
        func imageInfo(_ shot: XCUIScreenshot) -> [String: Any]
        {
            ["imageSize": NSCoder.string(for: shot.image.size), "imageOrientation": shot.image.imageOrientation.rawValue,
             "pixelWidth": shot.image.cgImage?.width ?? -1, "pixelHeight": shot.image.cgImage?.height ?? -1]
        }
        let data: [String: Any] = ["appFrame": NSCoder.string(for: app.frame), "windowFrame": NSCoder.string(for: window.frame),
            "composerFrame": NSCoder.string(for: input.frame), "backFrame": NSCoder.string(for: back.frame),
            "microphoneFrame": NSCoder.string(for: microphone.frame), "sendFrame": NSCoder.string(for: send.frame),
            "deviceOrientation": XCUIDevice.shared.orientation.rawValue,
            "appScreenshot": imageInfo(appShot), "windowScreenshot": imageInfo(windowShot), "screenScreenshot": imageInfo(screenShot)]
        let diagnostic = XCTAttachment(data: try JSONSerialization.data(withJSONObject: data, options: [.prettyPrinted, .sortedKeys]), uniformTypeIdentifier: "public.json")
        diagnostic.name = "Landscape-Frames-And-Image-Geometry"; diagnostic.lifetime = .keepAlways; add(diagnostic)
    }

    func testLargeTextKeepsHomeAndSettingsUsable() throws
    {
        let app = launchConnected(arguments: ["-UIPreferredContentSizeCategoryName", "UICTContentSizeCategoryAccessibilityXL"])
        XCTAssertTrue(app.buttons["settings"].waitForExistence(timeout: 20))
        XCTAssertTrue(app.buttons["chat-mobile-root"].waitForExistence(timeout: 20))
        XCTAssertTrue(app.buttons["newChat"].isHittable)
        capture(app, "Home-Large-Text")
        app.buttons["settings"].tap()
        XCTAssertTrue(app.buttons["settingsLimits"].waitForExistence(timeout: 10))
        app.buttons["settingsLimits"].tap()
        XCTAssertTrue(app.staticTexts["Weekly"].waitForExistence(timeout: 10))
        capture(app, "Usage-Large-Text")
    }

    /// The harness writes a corrupt manifest into this task-owned simulator
    /// container before launch. Product code receives no synthetic draft state.
    func testUnreadableLocalDraftRequiresExplicitDiscard() throws
    {
        let environment = ProcessInfo.processInfo.environment
        guard let namespace = environment["UAM_CORRUPT_DRAFT_TEST_NAMESPACE"], !namespace.isEmpty else
        { throw XCTSkip("Local draft recovery is opt-in after pre-seeding a corrupt manifest in an isolated simulator namespace.") }
        let newChat = environment["UAM_CORRUPT_DRAFT_TEST_KIND"] == "new"
        let app = XCUIApplication()
        app.launchEnvironment["UAM_TEST_ENDPOINT"] = "http://127.0.0.1:58962"
        app.launchEnvironment["UAM_TEST_TOKEN"] = "uam-isolated-ui-test-token"
        app.launchEnvironment["UAM_TEST_DRAFT_NAMESPACE"] = namespace
        app.launch()
        if newChat
        {
            XCTAssertTrue(app.buttons["newChat"].waitForExistence(timeout: 20))
            app.buttons["newChat"].tap()
            XCTAssertTrue(app.buttons["project-mobile-project"].waitForExistence(timeout: 10))
            app.buttons["project-mobile-project"].tap()
        }
        else
        {
            XCTAssertTrue(app.buttons["Chats"].waitForExistence(timeout: 20))
            app.buttons["Chats"].tap()
            XCTAssertTrue(app.buttons["chat-mobile-root"].waitForExistence(timeout: 10))
            app.buttons["chat-mobile-root"].tap()
        }
        let retry = app.buttons["retrySavedDraft"]
        let discard = app.buttons["discardUnreadableDraft"]
        let input = app.textViews["composerInput"]
        XCTAssertTrue(retry.waitForExistence(timeout: 15))
        XCTAssertTrue(discard.exists)
        XCTAssertFalse(input.isEnabled)
        XCTAssertFalse(app.buttons["sendMessage"].isEnabled)
        retry.tap()
        XCTAssertTrue(retry.exists, "A failed read must keep the original local draft blocked.")
        XCTAssertFalse(input.isEnabled)
        capture(app, "Unreadable-Draft-Blocked")

        discard.tap()
        XCTAssertTrue(app.alerts.buttons["confirmDiscardUnreadableDraft"].firstMatch.waitForExistence(timeout: 5))
        app.buttons["Keep draft"].tap()
        XCTAssertTrue(retry.exists, "Cancel must retain the unreadable draft and its write gate.")
        discard.tap()
        XCTAssertTrue(app.alerts.buttons["confirmDiscardUnreadableDraft"].firstMatch.waitForExistence(timeout: 5))
        app.alerts.buttons["confirmDiscardUnreadableDraft"].firstMatch.tap()
        let recovered = NSPredicate { _, _ in input.exists && input.isEnabled && !retry.exists }
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(predicate: recovered, object: nil)], timeout: 10), .completed)
        XCTAssertEqual(input.value as? String ?? "", "")
        XCTAssertFalse(app.buttons["Dismiss error"].exists)

        let marker = "recovered-local-draft-" + UUID().uuidString.lowercased()
        let replies = app.staticTexts.matching(NSPredicate(format: "label == %@", "The isolated host received your message."))
        let before = replies.count
        input.tap(); input.typeText(marker)
        app.buttons["sendMessage"].tap()
        let sent = NSPredicate { _, _ in app.staticTexts[marker].exists && replies.count > before && (input.value as? String ?? "").isEmpty }
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(predicate: sent, object: nil)], timeout: 30), .completed)
        XCTAssertFalse(app.buttons["Dismiss error"].exists)
        capture(app, newChat ? "New-Chat-Local-Draft-Recovered" : "Chat-Local-Draft-Recovered")
    }

    /// Requires a real HEIC fixture imported with simctl addmedia on this
    /// task's simulator. No product picker or attachment injection is used.
    func testPhotosLibraryOriginalAndCompatibleImage() throws
    {
        let environment = ProcessInfo.processInfo.environment
        guard environment["UAM_HEIC_TEST_ENABLED"] == "1",
              let photoLabel = environment["UAM_HEIC_TEST_PHOTO_LABEL"], !photoLabel.isEmpty else
        { throw XCTSkip("Photos-library HEIC journey requires opt-in and the observed accessibility label of the seeded HEIC photo.") }
        XCUIDevice.shared.orientation = .portrait
        let app = launchConnected()
        XCTAssertTrue(app.buttons["Chats"].waitForExistence(timeout: 20))
        app.buttons["Chats"].tap()
        XCTAssertTrue(app.buttons["chat-mobile-root"].waitForExistence(timeout: 20))
        app.buttons["chat-mobile-root"].tap()
        XCTAssertTrue(app.buttons["addAttachment"].waitForExistence(timeout: 10))
        app.buttons["addAttachment"].tap()
        XCTAssertTrue(app.buttons["Photo library"].waitForExistence(timeout: 5))
        app.buttons["Photo library"].tap()

        let photo = app.images[photoLabel]
        guard photo.waitForExistence(timeout: 15) else
        {
            capture(app, "PhotosPicker-Diagnostic")
            let hierarchy = XCTAttachment(string: app.debugDescription)
            hierarchy.name = "PhotosPicker-Accessibility-Diagnostic"; hierarchy.lifetime = .keepAlways; add(hierarchy)
            XCTFail("The actual PhotosPicker did not expose a selectable photo image. Inspect the diagnostic; no synthetic picker was substituted.")
            return
        }
        capture(app, "PhotosPicker-Actual-HEIC-Library")
        // iOS 26 Photos exposes a visible asset image with hit point {-1,-1}.
        // Its measured frame and observed exact label identify the seeded
        // fixture; derive the tap from that real element rather than a guess.
        XCTAssertGreaterThan(photo.frame.width, 0)
        XCTAssertGreaterThan(photo.frame.height, 0)
        photo.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.5)).tap()
        let preservation = app.staticTexts["Original photo kept as a file; JPEG added for agents."]
        if !preservation.waitForExistence(timeout: 3)
        {
            let addPhotos = app.buttons.matching(NSPredicate(format: "(label BEGINSWITH %@ AND label != %@) OR label == %@", "Add", "Add attachment", "Done")).firstMatch
            guard addPhotos.waitForExistence(timeout: 5) else
            {
                let hierarchy = XCTAttachment(string: app.debugDescription)
                hierarchy.name = "PhotosPicker-Completion-Diagnostic"; hierarchy.lifetime = .keepAlways; add(hierarchy)
                XCTFail("The actual picker must expose its native completion control or finish the import.")
                return
            }
            addPhotos.tap()
        }

        XCTAssertTrue(preservation.waitForExistence(timeout: 20), "The selected HEIC must yield both an untouched file and compatible JPEG.")
        let removeOriginal = app.buttons.matching(NSPredicate(format: "label BEGINSWITH %@ AND (label ENDSWITH[c] %@ OR label ENDSWITH[c] %@)", "Remove ", ".heic", ".heif")).firstMatch
        XCTAssertTrue(removeOriginal.waitForExistence(timeout: 10))
        let originalName = String(removeOriginal.label.dropFirst("Remove ".count))
        let compatibleName = (originalName as NSString).deletingPathExtension + "-compatible.jpg"
        XCTAssertTrue(app.buttons["Remove " + compatibleName].exists)
        XCTAssertTrue(app.staticTexts["HEIC"].exists || app.staticTexts["HEIF"].exists)
        XCTAssertTrue(app.staticTexts["JPG"].exists, "The original and agent-compatible image should have distinct type labels.")
        capture(app, "HEIC-Original-And-Compatible-Draft")

        let marker = "native-photos-heic-round-trip-" + UUID().uuidString.lowercased()
        let input = app.textViews["composerInput"]
        input.tap(); input.typeText(marker)
        let replies = app.staticTexts.matching(NSPredicate(format: "label == %@", "The isolated host received your message."))
        let repliesBeforeSending = replies.count
        app.buttons["sendMessage"].tap()
        let pairedMessage = NSPredicate { _, _ in
            app.staticTexts[marker].exists && app.staticTexts[originalName].exists && app.staticTexts[compatibleName].exists &&
                replies.count > repliesBeforeSending && (input.value as? String ?? "").isEmpty && !app.buttons["Remove " + originalName].exists
        }
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(predicate: pairedMessage, object: nil)], timeout: 30), .completed)
        XCTAssertFalse(app.buttons["Dismiss error"].exists)
        app.buttons["Back to chats"].tap()
        XCTAssertTrue(app.buttons["chat-mobile-root"].waitForExistence(timeout: 10))
        app.buttons["chat-mobile-root"].tap()
        XCTAssertTrue(app.staticTexts[originalName].waitForExistence(timeout: 15))
        XCTAssertTrue(app.staticTexts[compatibleName].exists, "Both staged descriptors must survive a fresh transcript load.")
        capture(app, "HEIC-Paired-Attachments-Reopened")
        let evidence: [String: Any] = ["test": "testPhotosLibraryOriginalAndCompatibleImage", "prompt": marker,
                                       "originalName": originalName, "compatibleName": compatibleName,
                                       "source": "Actual PhotosPicker with seeded HEIC", "reopenedWithBothAttachments": true]
        let receipt = XCTAttachment(data: try JSONSerialization.data(withJSONObject: evidence, options: [.prettyPrinted, .sortedKeys]), uniformTypeIdentifier: "public.json")
        receipt.name = "PhotosPicker-HEIC-Round-Trip-Receipt"; receipt.lifetime = .keepAlways; add(receipt)
    }

    /// Opt-in integration against the isolated real CEF host created by
    /// tests/companion_native_smoke.py. Normal deterministic UI runs skip it.
    func testRealCompanionRoundTrip() throws
    {
        let environment = ProcessInfo.processInfo.environment
        guard let endpoint = environment["UAM_REAL_TEST_ENDPOINT"], !endpoint.isEmpty,
              let token = environment["UAM_REAL_TEST_TOKEN"], !token.isEmpty,
              let chatID = environment["UAM_REAL_TEST_CHAT_ID"], !chatID.isEmpty else
        { throw XCTSkip("Real companion round trip requires an explicitly supplied isolated endpoint, token and chat ID.") }

        XCUIDevice.shared.orientation = .portrait
        let marker = environment["UAM_REAL_TEST_MARKER"] ?? ("native-real-cef-round-trip-" + UUID().uuidString.lowercased())
        let app = XCUIApplication()
        app.launchEnvironment["UAM_TEST_ENDPOINT"] = endpoint
        app.launchEnvironment["UAM_TEST_TOKEN"] = token
        app.launchEnvironment["UAM_TEST_DRAFT_NAMESPACE"] = UUID().uuidString
        app.launch()

        let chats = app.buttons["Chats"]
        XCTAssertTrue(chats.waitForExistence(timeout: 30), "The native home screen must load from the isolated real host.")
        chats.tap()
        let chat = app.buttons["chat-" + chatID]
        XCTAssertTrue(chat.waitForExistence(timeout: 30), "The supplied isolated chat must be published by the real CEF companion.")
        if !chat.isHittable { app.swipeUp() }
        XCTAssertTrue(chat.isHittable)
        chat.tap()
        let input = app.textViews["composerInput"]
        XCTAssertTrue(input.waitForExistence(timeout: 20))
        XCTAssertEqual(input.value as? String ?? "", "", "The test starts with a separate empty draft namespace.")
        XCTAssertFalse(app.buttons["Dismiss error"].exists)
        capture(app, "Real-CEF-Before-Send")

        input.tap()
        input.typeText(marker)
        let send = app.buttons["sendMessage"]
        XCTAssertTrue(send.isEnabled)
        let replies = app.staticTexts.matching(NSPredicate(format: "label == %@", "Companion round trip."))
        let repliesBeforeSending = replies.count
        send.tap()

        // Earlier protocol checks may already have produced the same fixture
        // answer. Require another answer and this unique submitted marker;
        // the smoke runner separately verifies the fake-provider log and saved
        // user message. Stable queries avoid indexing a changing transcript.
        let repliedToThisMessage = NSPredicate { _, _ in
            app.staticTexts[marker].exists && replies.count > repliesBeforeSending
        }
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(predicate: repliedToThisMessage, object: nil)], timeout: 60), .completed,
                       "The actual fake provider must answer the message sent through the native app and real CEF companion.")
        let settled = NSPredicate { _, _ in
            input.exists && (input.value as? String ?? "").isEmpty && send.exists && !send.isEnabled
        }
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(predicate: settled, object: nil)], timeout: 20), .completed,
                       "An acknowledged send must clear only its submitted draft and return to idle.")
        XCTAssertFalse(app.buttons["Dismiss error"].exists, "The completed real-host round trip must not leave a connection or send error.")
        capture(app, "Real-CEF-Native-Round-Trip")

        let evidence: [String: Any] = ["test": "testRealCompanionRoundTrip", "chatId": chatID, "prompt": marker,
                                       "expectedAnswer": "Companion round trip.", "draftCleared": (input.value as? String ?? "").isEmpty,
                                       "errorVisible": app.buttons["Dismiss error"].exists, "idle": !send.isEnabled]
        let receipt = XCTAttachment(data: try JSONSerialization.data(withJSONObject: evidence, options: [.prettyPrinted, .sortedKeys]), uniformTypeIdentifier: "public.json")
        receipt.name = "Real-CEF-Native-Round-Trip-Receipt"; receipt.lifetime = .keepAlways; add(receipt)
    }

    private func capture(_ app: XCUIApplication, _ label: String)
    {
        let attachment = XCTAttachment(screenshot: app.screenshot())
        attachment.name = label; attachment.lifetime = .keepAlways
        add(attachment)
    }
}
