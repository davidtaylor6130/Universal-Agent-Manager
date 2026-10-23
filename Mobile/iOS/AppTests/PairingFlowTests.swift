import XCTest
import UAMKit
@testable import UAM

@MainActor final class PairingFlowTests: XCTestCase
{
    @MainActor private final class Checks
    {
        var details: [PairingDetails] = []
        var waiting: [CheckedContinuation<Void, any Error>] = []
        func check(_ value: PairingDetails) async throws
        {
            details.append(value)
            try await withCheckedThrowingContinuation { waiting.append($0) }
        }
        func finish(_ index: Int, error: (any Error)? = nil)
        {
            if let error { waiting[index].resume(throwing: error) }
            else { waiting[index].resume() }
        }
    }

    private func flow() -> PairingFlow
    {
        let flow = PairingFlow()
        flow.name = "My Mac"; flow.address = "https://main.example:58950/companion"; flow.token = "fixture-only"
        return flow
    }
    private func waitForChecks(_ count: Int, _ checks: Checks) async
    {
        for _ in 0..<1000 { if checks.waiting.count >= count { return }; await Task.yield() }
        XCTFail("Expected suspended fixture checks")
    }
    private func waitForResult(_ flow: PairingFlow) async
    {
        for _ in 0..<1000 { if flow.phase != .checking { return }; await Task.yield() }
        XCTFail("Pairing did not produce a result")
    }

    func testManualSubmitEntersCheckingSynchronouslyAndAllowsOnlyOneAttempt() async
    {
        let flow = flow(), checks = Checks()
        flow.submit(using: checks.check)
        XCTAssertEqual(flow.phase, .checking)
        flow.submit(using: checks.check)
        await waitForChecks(1, checks)
        XCTAssertEqual(checks.details.count, 1)
        XCTAssertEqual(checks.details[0].token, "fixture-only")
        checks.finish(0)
        await waitForResult(flow)
        XCTAssertEqual(flow.phase, .success)
        XCTAssertEqual(flow.checkedName, "My Mac")
        for _ in 0..<5 { await Task.yield() }
        XCTAssertEqual(flow.phase, .success, "Success must remain until explicit Close")
    }

    func testValidQRAndDeepLinkStartCheckingWithoutAnotherTap() async
    {
        for code in [#"{"type":"uam-companion","version":1,"url":"https://main.example:58950","token":"fixture-QR","name":"QR Mac"}"#,
                     "uam://pair?url=https%3A%2F%2Fmain.example%3A58950&token=fixture-QR&name=QR%20Mac"]
        {
            let flow = PairingFlow(), checks = Checks()
            XCTAssertTrue(flow.acceptCode(code, using: checks.check))
            XCTAssertEqual(flow.phase, .checking)
            await waitForChecks(1, checks)
            XCTAssertEqual(checks.details[0].name, "QR Mac")
            checks.finish(0)
            await waitForResult(flow)
            XCTAssertEqual(flow.phase, .success)
        }
    }

    func testFailureRemainsVisibleAndEditDetailsRetainsInput() async
    {
        let flow = flow(), checks = Checks()
        flow.submit(using: checks.check)
        await waitForChecks(1, checks)
        checks.finish(0, error: CompanionError.network(code: -1001))
        await waitForResult(flow)
        XCTAssertEqual(flow.phase, .failure)
        XCTAssertEqual(flow.problem?.title, "Connection timed out")
        XCTAssertFalse(flow.problem!.message.localizedCaseInsensitiveContains("chat"))
        flow.editDetails()
        XCTAssertEqual(flow.phase, .entry)
        XCTAssertEqual(flow.address, "https://main.example:58950/companion")
        XCTAssertEqual(flow.token, "fixture-only")
        XCTAssertNil(flow.problem)
    }

    func testCancelledAndStaleChecksCannotReplaceNewAttemptResult() async
    {
        let flow = flow(), checks = Checks()
        flow.submit(using: checks.check)
        await waitForChecks(1, checks)
        flow.cancel()
        XCTAssertEqual(flow.phase, .entry)
        flow.name = "New attempt"
        flow.submit(using: checks.check)
        await waitForChecks(2, checks)
        checks.finish(0) // A deliberately cancellation-unaware fixture returns late.
        for _ in 0..<5 { await Task.yield() }
        XCTAssertEqual(flow.phase, .checking)
        checks.finish(1)
        await waitForResult(flow)
        XCTAssertEqual(flow.phase, .success)
        XCTAssertEqual(flow.checkedName, "New attempt")
    }

    func testInvalidQRHasAResultButNeverStartsNetworkCheck() async
    {
        let flow = flow(), checks = Checks()
        XCTAssertFalse(flow.acceptCode("untrusted fixture text", using: checks.check))
        XCTAssertEqual(flow.phase, .failure)
        XCTAssertTrue(checks.details.isEmpty)
        XCTAssertEqual(flow.token, "fixture-only")
    }

    func testDismissalInvalidationPreservesBothTerminalPages() async
    {
        for failure in [false, true]
        {
            let flow = flow(), checks = Checks()
            flow.submit(using: checks.check)
            await waitForChecks(1, checks)
            checks.finish(0, error: failure ? CompanionError.network(code: -1001) : nil)
            await waitForResult(flow)
            let title = flow.problem?.title
            flow.invalidate(); flow.invalidate() // Close, followed by onDisappear.
            XCTAssertEqual(flow.phase, failure ? .failure : .success)
            XCTAssertEqual(flow.problem?.title, title)
            XCTAssertEqual(flow.checkedName, "My Mac")
            XCTAssertEqual(flow.checkedAddress, "https://main.example:58950")
        }
    }

    func testDismissalInvalidationCancelsWithoutShowingEntryOrAcceptingLateResult() async
    {
        let flow = flow(), checks = Checks()
        flow.submit(using: checks.check)
        await waitForChecks(1, checks)
        flow.invalidate()
        XCTAssertEqual(flow.phase, .checking)
        checks.finish(0)
        for _ in 0..<10 { await Task.yield() }
        XCTAssertEqual(flow.phase, .checking)
        XCTAssertNil(flow.problem)
    }

    func testBusyAndTerminalDeepLinksAreConsumedWithoutStartingAfterEdit() async throws
    {
        let flow = flow(), checks = Checks()
        let url = try XCTUnwrap(URL(string: "uam://pair?url=https%3A%2F%2Fanother.example&token=another-fixture"))
        flow.submit(using: checks.check)
        await waitForChecks(1, checks)
        var pending: URL? = url
        XCTAssertFalse(flow.consumePendingCode(&pending, using: checks.check))
        XCTAssertNil(pending)
        checks.finish(0, error: CompanionError.network(code: -1001))
        await waitForResult(flow)
        pending = url
        XCTAssertFalse(flow.consumePendingCode(&pending, using: checks.check))
        XCTAssertNil(pending)
        flow.editDetails()
        XCTAssertFalse(flow.consumePendingCode(&pending, using: checks.check))
        for _ in 0..<10 { await Task.yield() }
        XCTAssertEqual(flow.phase, .entry)
        XCTAssertEqual(checks.details.count, 1)
        XCTAssertEqual(flow.address, "https://main.example:58950/companion")

        // Opening the link again from entry is a new, explicit incoming event.
        pending = url
        XCTAssertTrue(flow.consumePendingCode(&pending, using: checks.check))
        XCTAssertNil(pending)
        await waitForChecks(2, checks)
        checks.finish(1)
        await waitForResult(flow)
        pending = url
        XCTAssertFalse(flow.consumePendingCode(&pending, using: checks.check))
        XCTAssertNil(pending)
        XCTAssertEqual(flow.phase, .success)
        XCTAssertEqual(checks.details.count, 2)
    }

    func testPairingErrorsAreContextualAndNeverExposeUntrustedErrorText()
    {
        let cases: [(any Error, String)] = [
            (CompanionError.network(code: -1001), "Connection timed out"),
            (CompanionError.network(code: -1009), "This iPhone is offline"),
            (CompanionError.network(code: -1202), "Secure connection failed"),
            (CompanionError.http(status: 401), "Access token not accepted"),
            (CompanionError.http(status: 404), "Universal Agent Manager was not found"),
            (CompanionError.invalidResponse("raw-server-token-fixture"), "Unexpected response"),
            (NSError(domain: "fixture", code: 5, userInfo: [NSLocalizedDescriptionKey: "raw-server-token-fixture"]), "Connection could not be checked")
        ]
        for (error, title) in cases
        {
            let problem = PairingProblem(error)
            XCTAssertEqual(problem.title, title)
            XCTAssertFalse(problem.message.localizedCaseInsensitiveContains("chat"))
            XCTAssertFalse(problem.message.contains("raw-server-token-fixture"))
        }
        XCTAssertTrue(CompanionError.network(code: -1001).localizedDescription.contains("Check the chat"), "Uncertain chat mutation warnings must remain unchanged")
    }
}
