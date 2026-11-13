# Automation API

The automation API lets another program, such as an n8n workflow, start a UAM chat in an allowed workspace. It is served by the [phone access](phone-access.md) companion server, so it is only reachable while Phone access is on and UAM opens no additional network listener. It is a narrower interface than phone access: its tokens are separate from the companion token and cannot use the companion API, and the companion token cannot use this one. Like phone access, it refuses requests from programs UAM itself started, such as provider agents and terminals.

## Tokens

Tokens are created and revoked in [Automation settings](settings/automation.md). Each has a trust level, an allowed workspace list and the most Git access a request may ask for. The token is shown once, when it is created; UAM stores only its SHA-256 hash, and the desktop settings actions that manage tokens are not available to the phone. Every request re-reads the token file, so revocation applies to the next request. A revoked token cannot create chats or read status; chats it already created are kept, and a waiting one can still be started or deleted by the user.

Each token expires after the lifetime chosen when it was created: 30 days, 90 days (the default) or one year. An expired token is refused with 401 and a message saying it has expired. Regenerate, on the desktop only, replaces a token's secret while keeping its name, trust, workspaces, Git ceiling and chats, and restarts its lifetime. The old secret is refused from the next request, including status polls of chats it created, and the new one is shown once. Neither the companion nor the automation API can regenerate, create or revoke tokens. Tokens created before expiry existed receive the 90-day default, counted from the first time UAM reads them after the upgrade.

## Requests

`POST /automation/chats` takes `Authorization: Bearer <token>` and a JSON body of at most 256 KiB, and is refused if it carries a browser `Origin`.

| Field | Required | Meaning |
|---|---|---|
| `prompt` | Yes | First prompt, up to 128 KiB. |
| `workspaceId` | Yes | A UAM workspace folder ID on the token's allowed list, shown on the token's row in Settings. |
| `providerId` | Yes | A provider available on the workspace's host. Antigravity is refused; see below. |
| `maxIterations` | Yes | Integer from 1 to 200; see below. |
| `gitPermission` | Yes | `commit` or `pr`, no more than the token allows. |
| `modelId`, `title` | No | Otherwise the provider default and a generated title. |

An unknown, malformed or expired token returns 401, a workspace or Git permission outside the token returns 403, an invalid field including a missing `maxIterations` returns 400, an oversized body 413, a workspace that cannot be isolated or a provider that is unavailable 409, and 16 open chats for one token 429. None of these creates a chat, a worktree or a provider process. A trusted chat that is created but cannot start returns an error naming the chat, which stays waiting for the user. A successful response is `{"chatId": "...", "status": "running"}` for a trusted token, or `"awaitingApproval"` for an untrusted one.

`GET /automation/chats/<chatId>` returns `status` (`awaitingApproval`, `running`, `completed`, `blocked`, `paused` or `stopped`), `iterationsUsed`, `maxIterations` and `gitPermission`. `blocked` includes a chat whose worktree preparation failed after the send was accepted. A chat created by a different token reads as not found. Status is polling only: UAM makes no outbound callbacks or webhooks. The API does not return transcript content.

## Chat behaviour

Every automation chat is requested with its own Git worktree, and the request is refused if UAM cannot isolate that workspace ([the same rule as started chats](providers/uam-tools.md)). An SSH workspace turns out to have no Git repository only at launch; the provider start is then refused rather than run in the shared directory. A trusted token's chat starts immediately. An untrusted token's chat appears in [Active Chats](sidebar/active/active.md) as **Waiting for approval**, without Done or Snooze actions. Its prompt is shown as a message marked Not sent yet, with a question card beneath naming the token, iteration cap and Git access and offering **Start chat** on the desktop; there is no composer until it starts. Until the user starts it, sending, the structured provider process and CLI View are all refused. Start chat turns it into an ordinary chat in place: the prompt shows Sending and the sidebar shows Working through worktree and provider start, and the prompt becomes a normal message once the provider has it. A refused start returns the chat to waiting rather than losing the prompt; a workspace that fails after the start was accepted keeps the prompt as held input to resume.

`maxIterations` caps the chat's [goal](chat/goals.md) loop, and the lower of it and the Settings cap applies, including to stalled-loop resume. Automation chats always use the Default permission mode, ignoring a more permissive Settings default, so commands and edits still ask the user. **Implementation note:** `gitPermission` is enforced at the request against the token's ceiling and stated to the agent in the goal objective, so the visible message stays the prompt that was asked and every continuation repeats the policy; UAM does not block a Git command itself, so Default-mode approval is what stops an unapproved push. A provider that answers its own permission prompts (Antigravity) is not mediated by that approval, so requests for it are refused until a Git-aware gate exists.

## n8n example

Use an HTTP Request node to `POST https://<phone-address>/automation/chats` with a Header Auth credential of `Authorization: Bearer <token>` and this body, then a Wait node and a second HTTP Request node to `GET .../automation/chats/{{$json.chatId}}`, looping back to the Wait node until `status` is `completed`, `blocked`, `paused` or `stopped`.

```json
{"prompt": "Fix the failing parser test.", "workspaceId": "<folder-id>", "providerId": "codex-cli", "maxIterations": 5, "gitPermission": "commit"}
```

[Open decisions](documentation-review.md#automation-api) · [Security](security-enterprise.md).
