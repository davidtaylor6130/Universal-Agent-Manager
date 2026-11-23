# VCS adapter protocol, version 1

**Status:** specification, conformance runner and sample for the first slice of [issue #522](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/522). UAM does not launch adapters yet; nothing here is wired into the app or accepted. [David's decisions of 2026-11-23](../documentation-review.md#extension-adapters) set two rules this page builds on: an action button may only open a URL or a file, and each adapter executable is registered and approved once by the person in Settings.

An adapter lets someone add a version-control system to UAM without changing UAM. It is a separate program that UAM starts and talks to over standard input and output, in the same JSON-RPC style as ACP, so a crashing or misbehaving adapter can never take UAM down with it (no plugins load into UAM's process, as decided on 2026-10-25). Version 1 is read-only: an adapter can report status, diffs, history and branches, and offer buttons that open a web page or show a file. Writing operations come in a later version.

| File | Purpose |
|---|---|
| [`tools/vcs-adapter/conformance.py`](../../tools/vcs-adapter/conformance.py) | The conformance runner authors run against their adapter |
| [`tools/vcs-adapter/sample_folder_adapter.py`](../../tools/vcs-adapter/sample_folder_adapter.py) | A sample read-only adapter for a plain folder; it passes every check |
| [`tools/vcs-adapter/selftest.py`](../../tools/vcs-adapter/selftest.py) | Proves the runner passes the sample and catches each [deliberate fault](../../tools/vcs-adapter/fixtures/misbehaving_adapter.py); CTest `uam_vcs_adapter_conformance` |

## Registration and trust

The person registers an adapter in Settings with its executable, its arguments and a name. UAM never takes an adapter command from a repository, a chat or an agent, and refuses an executable or script that lies inside a registered workspace, because a repository's content is not trusted. Before the first launch, UAM shows the exact command line and asks for approval. The approval is bound to the command line and to a SHA-256 hash of the executable and of every argument that names an existing file, so `python3 adapter.py` covers `adapter.py`; if any of them changes, UAM asks again. Modules or files the adapter loads by itself are not covered, so a single-file executable or script is the safer form, and whether to go further is an [open question](#open-questions).

## Launch and transport

- **Process.** One adapter process per workspace that uses it, started on first use in an empty folder UAM owns, not in the workspace: a program the adapter runs by bare name must not be found in the repository. The workspace path arrives in `initialize`. The adapter runs in a Job Object that kills its members when closed on Windows, and in its own process group elsewhere, and stopping it stops everything it started. A process group does not hold a child that starts its own session (`setsid`), so on macOS and Linux UAM also stops the adapter's remaining descendants by walking the process tree, which is an open implementation detail.
- **Environment.** Only what a program needs to start: `PATH`, `HOME` or `USERPROFILE`, `LANG`, `LC_ALL`, `LC_CTYPE`, the temporary-folder variables, and on Windows `SYSTEMROOT`, `WINDIR`, `COMSPEC` and `PATHEXT`, plus `NoDefaultCurrentDirectoryInExePath=1` so Windows does not search the current folder for programs. No provider keys, tokens or proxy settings.
- **Framing.** JSON-RPC 2.0, one UTF-8 JSON object per line, ended by `\n`. No batches. Standard output carries only protocol messages; anything else there is a protocol violation. Standard error is for the adapter's own diagnostics: UAM drains it, keeps the last 64 KiB for the diagnostic log and never shows it in a chat.
- **Direction.** UAM sends requests; the adapter only answers them, one at a time in version 1. An adapter must not send requests or notifications. A notification from UAM (a message without `id`) needs no answer; version 1 defines none.
- **Shutdown.** UAM sends `shutdown`, the adapter answers `null`, UAM closes standard input and the adapter exits with code 0 within 2 s. An adapter must also exit within 2 s when its standard input closes without `shutdown`.

## Limits

| Limit | Value | When it is broken |
|---|---|---|
| Time to answer `initialize` | 5 s | UAM stops the adapter |
| Time to answer any other request | 10 s (sent as `limits.requestTimeoutMs`) | UAM stops the adapter |
| One message | 4 MiB (sent as `limits.maxMessageBytes`) | UAM stops reading and stops the adapter. An adapter keeps every reply under the limit it was sent: a diff whose reply would be larger answers `tooLarge`, and a status list is cut and marked `truncated` |
| Standard error kept | Last 64 KiB | Older output is dropped; never an error |
| Changed files in one status | 10,000, then `truncated: true` | — |
| Error message | One line, at most 200 characters | UAM shortens it to one line |
| Actions | 8 per adapter | UAM refuses the adapter |

When UAM stops an adapter, it shows one plain line through the notification system, for example "The Folder adapter stopped responding.", and marks that workspace's version-control status unavailable. It starts the adapter again only on the next request the person makes, never in a loop; after three stops within ten minutes it leaves the adapter off for that workspace until the person turns it back on in Settings.

## Handshake

The first request is always `initialize`:

```json
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{
  "protocolVersion":1,
  "client":{"name":"UAM","version":"5.2.0"},
  "workspace":{"path":"/absolute/workspace"},
  "limits":{"maxMessageBytes":4194304,"requestTimeoutMs":10000}}}
```

```json
{"jsonrpc":"2.0","id":1,"result":{
  "protocolVersion":1,
  "adapter":{"id":"folder-sample","name":"Folder (sample)","version":"1.0.0"},
  "capabilities":["status","diff"],
  "actions":[
    {"id":"show-folder","label":"Show folder","kind":"openFile","target":"."},
    {"id":"show-baseline","label":"Show baseline copy","kind":"openFile","context":["path"]}]}}
```

- `adapter.id` is 2–32 lowercase letters, digits or hyphens, starting with a letter; `git` and `svn` are reserved. `name` is one line of 1–40 characters, shown wherever UAM names the system.
- `capabilities` uses the stable names of UAM's [`VcsCapability`](../../src/common/vcs/vcs_provider_declaration.h) (`VcsCapabilityId`), so an adapter's declaration fills the same place as Git's and SVN's built-in `VcsProviderDeclaration`. Each name may appear once.
- A `protocolVersion` other than 1, an unknown name, a name version 1 does not allow, or more than 8 actions makes UAM refuse the adapter, with one line naming the problem.

| Capability | Version 1 | Methods |
|---|---|---|
| `status` | Allowed | `vcs/status` |
| `diff` | Allowed | `vcs/diff` |
| `history` | Allowed | `vcs/history` and `vcs/revisionDiff`, matching UAM's History, which includes one revision's patch for a path |
| `branchList` | Allowed | `vcs/branchList` |
| `commit`, `update`, `branchCreate`, `branchSwitch` | Not allowed; their methods must be refused | Defined in a later version |
| `chatComparison`, `revisionComparison`, `workspaceIsolation`, `originComparison` | Not allowed | UAM-side features with no adapter method yet |

## Methods

Every path an adapter returns is relative to the repository root, uses `/`, and contains no `.`, `..` or empty parts; that includes `previousPath` and the paths in history entries. Every method of a capability the adapter did not declare must fail with `-32601`, exactly like an unknown method. The result fields map to UAM's own structures, which UAM fills from them: it keeps its own status codes and repository-path forms.

**`vcs/detect`** (always): `{"path": "/absolute/folder"}` → `{"repository": true, "root": "/absolute/root"}` or `{"repository": false}`. The root contains the path. Called for the workspace and for unrelated folders; claiming a folder that is not a repository is a conformance failure. UAM never uses the reported root as a security boundary: action targets are always checked against the workspace.

**`vcs/status`** (`status`): `{"includeLineStats": false}` → `{"branchOrRevision": "…", "files": [{"path": "a.txt", "status": "modified"}], "truncated": false}`. `status` is one of `added`, `modified`, `deleted`, `renamed`, `untracked` or `conflicted`; a `renamed` entry adds `previousPath`. With `includeLineStats: true`, entries may add `additions` and `deletions` (non-negative integers) or `binary: true`, and list the same files. These map to UAM's `VcsChangedFile`.

**`vcs/diff`** (`diff`): `{"path": "a.txt"}` → `{"patch": "unified diff text", "binary": false}`, or `{"patch": "", "binary": true}`, or `{"patch": "", "tooLarge": true}` when the reply would not fit in `maxMessageBytes`. A path with no changes fails with `-32002`.

**`vcs/history`** (`history`): `{"limit": 50, "before": "cursor", "search": "text"}`, where `before` and `search` are optional and `limit` is 1–200 → `{"entries": [{"revision": "42", "author": "…", "date": "2026-11-23T10:00:00Z", "message": "…", "paths": [{"action": "M", "path": "a.txt"}]}], "next": "cursor or empty"}`. Newest first, at most `limit` entries, `date` in ISO 8601; a page never repeats a revision of an earlier page. These map to UAM's `VcsHistoryEntry`.

**`vcs/revisionDiff`** (`history`): `{"revision": "42", "path": "a.txt"}` → the same shape as `vcs/diff`, for the patch that revision applied to that path. A path the revision did not change fails with `-32002`.

**`vcs/branchList`** (`branchList`): `{}` → `{"branches": [{"name": "main", "current": true}]}`. Names are unique; at most one is current.

**`actions/resolve`**: see actions below.

**`shutdown`**: no params → `null`.

## Actions

An action is a button UAM shows for the adapter, for example "Open in web UI". It can only show something: there is no kind that runs a command, and neither kind can be used to start a program.

| Field | Rule |
|---|---|
| `id` | Same rule as `adapter.id`, unique within the adapter |
| `label` | One line, 1–32 characters |
| `kind` | `openUrl` or `openFile`; any other kind makes UAM refuse the adapter |
| `target` | For a fixed action: the URL or file |
| `context` | For an action that depends on what is selected: a list of `path` and/or `revision`. UAM calls `actions/resolve` with `{"actionId": "…", "context": {"path": "a.txt", "revision": "42"}}` when the button is pressed, and the adapter answers `{"kind": "openFile", "target": "…"}` |

An action has either `target` or `context`, never both.

- **`openUrl`** opens an absolute `http` or `https` URL with a host and no user name or password in the default browser.
- **`openFile`** shows a file or folder inside the workspace in the system file manager (Finder, File Explorer or the desktop's file manager): a folder is opened there, and a file is selected in its folder. UAM never hands a file to the operating system's default handler, because for a program or launcher (`.exe`, `.bat`, `.lnk`, `.url`, `.app`, `.command`, `.webloc`, `.desktop` and others) that would run it, and a repository's files are not trusted. The target is `.`, a normalized path relative to the workspace, or an absolute path inside it; UAM checks the text first, refusing network paths and anything that leaves the workspace, then checks again after resolving links, and the item must exist.

UAM checks every target, fixed or resolved, before using it, and shows one line instead when it fails. A resolved `kind` must match the declared one. An adapter must refuse an `actions/resolve` path that is not one of its own changed files or that leaves the workspace. Whether a file may instead be opened in its default application, for documents only, and whether URLs may use other schemes, such as one that opens a desktop client, are [open questions](#open-questions).

## Errors

Errors use JSON-RPC error objects. `message` is one plain line of at most 200 characters that UAM can show the person, prefixed with the adapter's name, through the notification system. It must not contain addresses (URLs or host names), credentials, stack traces or paths outside the workspace; the details belong on standard error, which goes to the diagnostic log.

| Code | Meaning |
|---|---|
| `-32700` | The line was not valid JSON (answer with `"id": null` and keep running) |
| `-32600` | Not a valid request, or a request before `initialize` |
| `-32601` | Unknown method, or a method of a capability the adapter did not declare |
| `-32602` | Invalid params |
| `-32603` | The adapter failed unexpectedly while handling the request |
| `-32001` | The folder is not a repository of this kind |
| `-32002` | The path has no changes (or no such item for an action) |

## Conformance

Run the runner with the adapter command exactly as it would be registered, with absolute paths, against a repository of the adapter's kind that has at least one changed file:

```bash
python3 tools/vcs-adapter/conformance.py --workspace /path/to/repository -- /absolute/path/to/adapter --its-args
```

The runner launches the adapter the way UAM will (the minimal environment, an empty start folder, the limits above) and in its own process group, prints one line per check and exits 0 only when none fails; `--json FILE` also writes the report. A stopped adapter's process tree is killed and the adapter is restarted for the remaining checks, so one fault does not hide others, and the last line it wrote to standard error is shown as a note. A result of the wrong shape, including one nested too deeply to read, fails the check it arrived in, and output is printed as UTF-8 whatever the console's code page. The runner kills a tree with `taskkill /T` on Windows rather than a Job Object, so there it cannot catch a child whose adapter has already exited; on macOS and Linux it kills the process group. `--allow-clean` accepts a workspace with no changes, at the cost of weaker checks. An adapter that wraps a system whose reads rewrite its own metadata should avoid that (for Git, `GIT_OPTIONAL_LOCKS=0` or `git --no-optional-locks`); where that is impossible, `--exclude .git` leaves the metadata folder out of the read-only comparison. The checks are:

| Check | Passes when |
|---|---|
| `handshake` | `initialize` answers in time with a valid version, adapter, capabilities and at most 8 actions |
| `detect` | The workspace is detected with a root containing it, and an empty unrelated folder is not |
| `capability:<name>` | Each declared capability's methods answer with valid results: status lists at least one changed file, with relative, normalized and unique paths; the first ten changed files each have a diff; a path with no changes fails with `-32002`; history pages respect the limit, do not repeat and list valid paths, and `vcs/revisionDiff` answers for listed files |
| `refuses:<name>` | Every method of each undeclared version 1 capability, and every writing method, fails with `-32601` |
| `actions` | Every fixed target passes UAM's checks against the workspace; every context action resolves for a changed file, to a target that passes and keeps its kind; context paths that leave the workspace are refused; an undeclared action is not resolved |
| `errors` | Unknown methods give `-32601`, invalid params `-32602`, and a malformed line `-32700` with the adapter still answering afterwards |
| `error-messages` | Every error message seen was one plain line of at most 200 characters with no URL or IP address (a bare host name cannot be told from other dotted text, so authors check that themselves) |
| `shutdown` | The adapter exits with code 0 within 2 s after `shutdown`, and within 2 s when its input closes |
| `read-only` | Nothing in the workspace changed during the run |

Crashing, hanging, oversized or unreadably nested output, writing anything else to standard output, answering with the wrong id and leaving a child process running are each contained: the first five fail the check in progress with the reason, the child is killed with the rest of the tree, and a flood of standard error is drained without blocking the adapter.

**Results, 2026-11-23** (Linux, Python 3.13): the sample adapter passes all 15 checks, including a diff too large for the self-test's 1 MiB message limit, which it answers with `tooLarge`. `selftest.py` passes 30 of 30 cases in about 41 s, and three copies run at once beside four busy processes also passed. Four cases must pass: the sample, a correct history implementation, an error message in Japanese printed through a `cp1252` console, and a standard-error flood of about 4 MB. Twenty-six faults must each fail the expected check: crash, hang, oversized message, standard-output noise, wrong id, an escaping status path, a malformed status field, an empty status, history with an escaping path, slow handshake, a declared reserved capability, a declared capability that fails, an undeclared capability answered, a `javascript:` URL action, an action file outside the workspace, a root of `/` with a fixed target at `/`, an action that never resolves, an action that resolves a hostile path, a run-command action, a multi-line error message, an error message with an address, a workspace write, ignoring shutdown, a grandchild process that must not outlive a hung adapter, a reply nested 100,000 levels deep, and a grandchild left behind by an adapter that then crashes. The last runs only on macOS and Linux, for the `taskkill` limit above. Not run on Windows or macOS.

## Later slices

- **In UAM:** an adapter-backed `VcsProvider` behind [`VcsCommitService`](../architecture/vcs-boundaries.md), which needs a capability list read at run time rather than only the built-in constant declarations; the registration form and approval in Settings; and the action buttons in the commit panel. All need Claude review and David's approval under [AGENTS.md](../../AGENTS.md). This depends on the remaining [#519](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/519) VCS interface work.
- **Protocol version 2:** writing operations (commit, update, branch creation and switching), cancellation of a long request, and progress.
- **Custom AI providers:** the matching recipe uses generic ACP ([#523](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/523)), not this protocol.
- **Recipe package:** a template project and an author-facing page built from this one.

## Open questions

- May `openFile` open a document in its default application, refusing program and launcher types on each system, or should it only ever show the item in the file manager, as above?
- Should an action URL be allowed to use a scheme other than `http` or `https`, such as one that opens a desktop client like a Perforce or Plastic GUI? The operating system would hand it to an installed program, which is closer to running a command.
- Should the approval also cover what an adapter loads by itself, for example by requiring a single-file executable or hashing a declared folder?
- Should adapters work on SSH workspaces, started on the remote host through the helper, or stay local only?
- Is the restart rule above (only on the next request; off after three stops in ten minutes) right?
