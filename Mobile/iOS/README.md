# UAM for iPhone

Native SwiftUI/UIKit client for the main UAM computer, targeting iOS 26 or later. It uses the existing authenticated companion `/api` connection; providers and downstream SSH continue running on the computer. The browser companion remains available.

## Build

Open `UAM.xcodeproj` in Xcode 26 or later. The checked-in project uses the local dependency-free `UAMKit` Swift package. Choose an iPhone simulator and run the UAM scheme. For a physical phone, select your own signing team and a suitable bundle identifier in Xcode; no signing identity or provisioning profile is checked in. App Store/TestFlight publication is not part of this worktree.

Regenerate the project after changing source membership with `xcodegen generate --spec Mobile/iOS/project.yml` from the repository root. XcodeGen is needed only for regeneration, not to open the checked-in project.

```sh
xcodebuild -project Mobile/iOS/UAM.xcodeproj -scheme UAM \
  -destination 'platform=iOS Simulator,name=UAM iPhone 17' \
  -derivedDataPath Builds/agent-ios CODE_SIGNING_ALLOWED=NO build
swift test --package-path Mobile/iOS/UAMKit
```

Keep native desktop builds in `Builds/agent` and iOS DerivedData in `Builds/agent-ios`; these use different toolchains and cannot share a build directory. Do not clean or recreate them after edits.

## Connect

Run the desktop companion from this branch, or a newer build advertising the same capabilities, for native uploads and shared settings. Older hosts disable unsupported controls. Enable Phone Access in the main desktop app and use its published HTTPS route. Choose Show pairing code there, then Add environment → Scan pairing code on the phone. Manual entry uses the same address and access token. The phone needs a route to the computer. A managed-proxy QR includes its public root certificate for this saved connection only; chain, hostname and validity checks remain enabled. Manual entry requires system certificate trust or a root already paired for that same HTTPS origin. Scanning or confirming details advances through checking to a persistent connected/failed result with Close and recoverable editing. The QR uses the existing shared credential, rather than issuing a new per-device account or expiring access code. The app stores its credential in Keychain. Forget removes this phone’s saved connection, not the computer’s chats or its shared access token.

## Isolated verification

`Checks/fixture_server.py` serves deterministic test chats on loopback port 58962 without provider calls or production data. Debug UI tests inject its endpoint through `UAM_TEST_ENDPOINT` and `UAM_TEST_TOKEN`; Release does not support cleartext HTTP or fixture injection. Start the fixture, then run the UAM scheme’s tests against the selected simulator. The fixture retains fake mutations in memory, so restart its process between independent UI test invocations. For the large-catalogue journey, also start a separate instance with `--port 58965 --catalogue-size 1277`; the ordinary instance remains on 58962. The organisation journeys use another instance with `--organisation` on 58966; snapshot-independent snooze expiry uses `--expiry-check` on 58967. Start the expiry fixture fresh and let the app make its first initial-state request: that request starts a 12-second deadline, and later snapshot reads deliberately fail. Unit tests cover persisted draft settlement, immutable attachment blobs, malformed-draft recovery, navigation races, HEIC conversion and transcript paging; UAMKit tests cover the shared transport. Test fixture outcomes do not establish physical camera, LAN certificate trust, voice or real-provider acceptance.

Recorded checks and delivery limits: [Verification](VERIFICATION.md).

Product behaviour and current delivery limits: [Native phone agreement](../../docs/phone-native.md). Exact verification, Claude review and David’s acceptance remain separate in the [V5 log](<../../V5 Improvement Change log.md>).
