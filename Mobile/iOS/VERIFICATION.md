# Native iOS verification

The implementation lives on `t3/mobile-app-redesign`, derived from `codex/v5-documentation-contract` through merge `6c5ad9aa`. These checks establish implementation evidence, not David’s acceptance. The tracked [V5 log](<../../V5 Improvement Change log.md>) remains authoritative for review and sign-off.

## Verified boundaries

- The native SwiftUI/UIKit app uses the existing authenticated companion `/api`; it adds no independent provider backend or account service.
- All six provider implementations remain present. Provider execution, credentials, persistence and downstream SSH remain on the main computer.
- Phone selection, creation, branching, child opening and unrelated deletion preserve desktop selection and unsaved desktop drafts.
- HTTPS and normal certificate/hostname verification are required in Release. Cleartext loopback and disposable fixture injection are Debug-only.
- Sending and transfer commit are never automatically retried. Uncertain results retain the draft.
- Protected drafts have connection/project/chat ownership, revision-aware settlement and immutable attachment blobs. Unreadable drafts stay blocked until a successful retry or explicit local-only discard confirmation; both chat and new-project recovery/send journeys passed. HEIC/HEIF imports retain their original file and add an upright, bounded JPEG for provider compatibility.

## Automated checks

The iPhone 17 simulator runs iOS 26.5 with Xcode 27.1; the deployment target is iOS 26. Tests use disposable data and fake providers; they make no paid live-provider calls.

| Check | Evidence |
|---|---|
| UAMKit transport | 26 Debug and 25 Release tests passed; all 21 original transport cases retained |
| Frontend | 1,868 functional tests and 23 performance cases passed; production build passed |
| Native app units | 54 simulator XCTest cases passed, including explicit unreadable-draft recovery, pairing identity, draft integrity, navigation races and real HEIC encoding |
| Native UI journeys | Eleven main journeys passed together, including actual code panning, fitting-table cues, landscape reading width, background/dismiss/reopen drafts, child navigation and stop while retaining typed text; three opt-in journeys skip the default suite |
| Device build | Unsigned and development-signed iPhone Release builds passed; testing IPA exported and strict deep signature verification passed; Debug fixture injection markers are absent |
| Real desktop connection | Native UI → UAMKit → real CEF companion → fake ACP provider passed again after the GUI follow-up; original image preview and desktop-selection guards passed |
| System photo picker | Actual PhotosPicker HEIC import/send/reopen journey passed, then passed again with distinct original/JPEG type labels |
| Complete native inventory | 2,404/2,449 passed; 45 failed; four additional failures passed in isolation, while all 41 earlier failures repeated |
| Connection reuse | A real HTTP/1.1 fixture measured 857 upload requests on one connection, versus 857 connections before; iPhone/WAN timing is unverified |
| Shared companion protocol | 26 policy/transfer/settings/queue cases passed |
| Connected native handlers | Six focused CEF cases passed, including staging while the background file queue is held |
| Native auxiliary gates | 46/46 non-core CTest gates passed |

## Acceptance still pending

Claude’s first two bounded GUI attempts were rate-limited; focused read-only rounds three and four completed with no blocking critical defect. The follow-up accepted overflow cues, tested landscape bounds, attachment labels and the reference-sized composer; settings icon alignment was corrected afterwards using its prescribed fixed-width frame. The reviewed captures include home, conversation, keyboard, photo draft, new chat, settings, usage, manual pairing, recovery and the isolated desktop QR panel. The follow-up relied on the root’s landscape bounds evidence rather than independently opening that capture. This is scoped GUI review, not physical or David acceptance. Installation and launch on David’s physical iPhone 14 Pro Max passed through CoreDevice, and David confirmed the visible app on 17 October 2026. Physical camera, microphone/on-device recognition, QR scanning, trusted LAN/tailnet HTTPS and hardware keyboard remain unverified. A development-signed testing IPA was exported from source `c16fee25`; its profile covers one registered device and expires on 24 October 2026 at 20:07 UTC. The earlier simulator review had no physical-device access; David subsequently authorized and connected his phone for direct installation and launch. The shared Windows native build was not run on this Mac. David’s approval is limited to installation and visible launch; broader feature acceptance remains pending.

Saved connections use explicit QR/manual pairing. Automatic LAN discovery, embedded Tailscale networking, push notifications, cloud relay and App Store/TestFlight publication are outside this implementation. The optional CLI view is bounded plain output/input for an existing desktop session, rather than a full terminal emulator.

The repository’s complete native inventory is a separate gate and is not represented by the focused passes above. Its remaining failures and the source-triage limits are retained in the V5 log and task evidence.

[Build and connect](README.md) · [Native agreement](../../docs/phone-native.md) · [Exact shared protocol](../../docs/reference/companion-protocol.md).


## V5 integration and desktop installation

On 17 October 2026, merge `1a7858bd` integrates the native companion with newer V5 usage/OAuth/window changes. The signed desktop `4.11.0-alpha-120` package is installed and reopened from the user Applications folder with matching helpers; native AX inspection confirms Pair iPhone controls. Settings, 41 queue journals and chat metadata were unchanged during the atomic replacement. The saved LAN address is stale and QR reveal remains disabled until connection configuration is updated; no publication setting was changed. Native iOS source, kit, tests and Xcode project still match`c16fee25`, so this desktop rebuild does not extend the iPhone development profile.

Merged checks pass1878 frontend unit + 23 performance, 26 protocol, 8 lifecycle and 7 connected CEF cases, a complete real companion/fake-provider round trip and 46 non-core CTests. The full native gate remains failed at 2,409/2,451 with 42 previously recorded failure names; its one additional case against the older 41-failure run passes an unchanged isolated rerun. Initial disk/fixture/timing/live failures and their follow-up receipts remain retained. Integration, build and installation are distinct from broader physical or user acceptance. Detailed evidence is in `.UAM/AINotes/v5-mobile-integration/` and the tracked V5 log.
