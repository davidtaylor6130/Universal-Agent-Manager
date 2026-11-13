"""Driven packaged-app metrics for scripts/perf_suite.py (piece `driven`): the packaged app on a synthetic data root, a
deterministic fake ACP provider and the Chrome DevTools Protocol, measured end to end (CEF, native bridge, React and paint).

Each scenario runs alone with --only, so an optimisation can rerun the one metric it touches. No real provider runs: every
provider executable on PATH is the fake from tests/companion_native_smoke.py, and only OpenCode is turned on.

DevTools: the app already passes its command line to CEF, so Chromium's stock --remote-debugging-port switch works on any
build; this script passes it with a free loopback port. Chromium binds that port to 127.0.0.1 only. While the app runs, any
local process can drive it through the port, which is why the app runs on a disposable data root with a hermetic
environment and is stopped at the end. A normal launch never passes the switch.
"""
import base64
import json
import math
import os
import random
import shutil
import socket
import statistics
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from pathlib import Path

import perf_suite as suite

FAKE = suite.ROOT / 'tests' / 'companion_native_smoke.py'
PROVIDER = 'opencode-cli'
SHIMS = ('opencode', 'codex', 'gemini', 'claude', 'copilot', 'agy', 'antigravity')
SCENARIOS = ('new_thread', 'first_word', 'long_thread', 'typing', 'per_thread')
STREAM_CHUNK_MS = 25  # two words per delta: about 80 tokens/s per agent, the reference stream rate
SHORT_CHAT, LONG_CHAT = 'perf-chat-0001', 'perf-chat-0000'
LONG_PAGE = 'Message 19'  # every message on the 2,000-message chat's newest page (1950-1999) starts with it


# ---------------------------------------------------------------------------- DevTools protocol (standard library only)

class Cdp:
    """Minimal DevTools client for the app's page target: text frames, commands answered in order."""

    def __init__(self, port, timeout, process):
        deadline = time.time() + timeout
        target = None
        while time.time() < deadline and not target:
            if process.poll() is not None:
                raise RuntimeError(f'The app exited during startup with code {process.returncode}.')
            try:
                with urllib.request.urlopen(f'http://127.0.0.1:{port}/json/list', timeout=2) as response:
                    pages = [t for t in json.load(response) if t.get('type') == 'page' and t.get('webSocketDebuggerUrl')]
                target = next((t for t in pages if t.get('url', '').startswith('uam://')), None)
            except OSError:
                pass
            if not target:
                time.sleep(0.25)
        if not target:
            raise RuntimeError('No uam:// DevTools page target appeared.')
        path = target['webSocketDebuggerUrl'].split(f':{port}', 1)[1]
        self.socket = socket.create_connection(('127.0.0.1', port), timeout=300)
        key = base64.b64encode(os.urandom(16)).decode()
        # No Origin header: Chromium checks --remote-allow-origins only for requests that send one.
        self.socket.sendall(f'GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                            f'Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n'.encode())
        header = b''
        while b'\r\n\r\n' not in header:
            header += self.socket.recv(1)
        if b' 101 ' not in header.split(b'\r\n', 1)[0]:
            raise RuntimeError('DevTools refused the WebSocket upgrade.')
        self.next_id = 0

    def _read(self, count):
        data = b''
        while len(data) < count:
            chunk = self.socket.recv(count - len(data))
            if not chunk:
                raise RuntimeError('DevTools connection closed.')
            data += chunk
        return data

    def _frame(self):
        message = b''
        while True:
            first, second = self._read(2)
            length = second & 0x7f
            if length == 126:
                length = struct.unpack('>H', self._read(2))[0]
            elif length == 127:
                length = struct.unpack('>Q', self._read(8))[0]
            payload = self._read(length)
            opcode = first & 0x0f
            if opcode == 9:  # ping
                self._send(payload, 0xA)
                continue
            if opcode == 8:
                raise RuntimeError('DevTools closed the connection.')
            message += payload
            if first & 0x80:
                return message

    def _send(self, payload, opcode=0x1):
        mask = os.urandom(4)
        length = len(payload)
        header = bytes([0x80 | opcode])
        header += bytes([0x80 | length]) if length < 126 else (bytes([0x80 | 126]) + struct.pack('>H', length) if length < 65536
                                                               else bytes([0x80 | 127]) + struct.pack('>Q', length))
        self.socket.sendall(header + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    def call(self, method, **params):
        self.next_id += 1
        self._send(json.dumps({'id': self.next_id, 'method': method, 'params': params}).encode())
        while True:
            message = json.loads(self._frame())
            if message.get('id') == self.next_id:
                if 'error' in message:
                    raise RuntimeError(f'{method}: {message["error"]}')
                return message.get('result', {})

    def js(self, expression):
        """Evaluates an expression in the page, awaiting a promise, and returns its JSON value."""
        result = self.call('Runtime.evaluate', expression=expression, awaitPromise=True, returnByValue=True)
        if 'exceptionDetails' in result:
            details = result['exceptionDetails']
            raise RuntimeError(f'Page script failed: {details.get("exception", {}).get("description", details.get("text"))}')
        return result.get('result', {}).get('value')

    def click(self, selector):
        """A left click at the element's centre through the browser's input pipeline, as a mouse would deliver it."""
        point = self.js(f'__perf.centre({json.dumps(selector)})')
        if not point:
            raise RuntimeError(f'Nothing to click for {selector}.')
        for kind in ('mouseMoved', 'mousePressed', 'mouseReleased'):
            self.call('Input.dispatchMouseEvent', type=kind, x=point[0], y=point[1], button='left' if kind != 'mouseMoved' else 'none',
                      clickCount=1 if kind != 'mouseMoved' else 0)

    def key(self, character=None, enter=False):
        """One key press (a letter, or Enter) through the browser's input pipeline, with the real key code."""
        if enter:
            down = {'key': 'Enter', 'code': 'Enter', 'windowsVirtualKeyCode': 13, 'nativeVirtualKeyCode': 13, 'text': '\r', 'unmodifiedText': '\r'}
        else:
            down = {'key': character, 'code': f'Key{character.upper()}', 'windowsVirtualKeyCode': ord(character.upper()),
                    'nativeVirtualKeyCode': ord(character.upper()), 'text': character, 'unmodifiedText': character}
        self.call('Input.dispatchKeyEvent', type='keyDown', **down)
        self.call('Input.dispatchKeyEvent', type='keyUp', **{k: v for k, v in down.items() if k not in ('text', 'unmodifiedText')})

    def close(self):
        try:
            self.socket.close()
        except OSError:
            pass


# Page helpers. Times are the page's performance.now(); a trigger's start is its input event's timeStamp (when the browser
# received it), captured by a window-level capture listener. afterPaint resolves once the next frame has been produced.
PAGE_HELPERS = r'''
window.__perf = window.__perf || (() => {
  const perf = {
    down: null,
    afterPaint: () => new Promise((resolve) => requestAnimationFrame(() => { const c = new MessageChannel(); c.port1.onmessage = () => resolve(performance.now()); c.port2.postMessage(0) })),
    until: (check, timeout = 30000, what = 'condition') => new Promise((resolve, reject) => {
      const started = performance.now()
      const test = () => {
        let value
        try { value = check() } catch (e) {}
        if (value) { observer.disconnect(); clearInterval(timer); resolve(true) }
        else if (performance.now() - started > timeout) { observer.disconnect(); clearInterval(timer); reject(new Error('timed out waiting for ' + what)) }
      }
      const observer = new MutationObserver(test)
      observer.observe(document.body, { childList: true, subtree: true, characterData: true, attributes: true })
      const timer = setInterval(test, 50)
      test()
    }),
    centre: (selector) => {
      const element = document.querySelector(selector)
      if (!element) return null
      element.scrollIntoView({ block: 'nearest' })
      const r = element.getBoundingClientRect()
      return [r.x + r.width / 2, r.y + r.height / 2]
    },
    composer: () => document.querySelector('textarea.uam-composer-textarea'),
    dialog: () => document.querySelector('[role="dialog"][aria-label="New chat"]'),
    create: () => [...document.querySelectorAll('[role="dialog"][aria-label="New chat"] button')].find((b) => b.textContent.trim() === 'Create chat'),
    assistantWith: (text) => [...document.querySelectorAll('article.conversation-turn--assistant')].some((a) => a.textContent.includes(text)),
    streaming: () => document.querySelectorAll('article[data-streaming]').length,
  }
  // The trigger is a click (dispatched on release, when the action runs) or an Enter keydown. Arming a wait clears it, so a
  // condition that already holds cannot resolve against an earlier trigger.
  for (const type of ['click', 'keydown']) window.addEventListener(type, (e) => { if (type === 'click' || e.key === 'Enter') perf.down = e.timeStamp }, true)
  const afterTrigger = async (check, what) => {
    perf.down = null
    await perf.until(() => perf.down !== null && check(), 30000, what)
    return (await perf.afterPaint()) - perf.down
  }
  // Since the trigger: the new chat's composer is ready to type into (another element, enabled, dialog closed).
  perf.newComposer = (old) => afterTrigger(() => { const c = perf.composer(); return c && c !== old && !c.disabled && !perf.dialog() }, 'the new chat composer')
  perf.textPainted = afterTrigger
  // Typing probe: keydown timeStamps queue up; each echoed character pops the oldest and resolves at the next frame.
  perf.typing = (composer) => {
    const state = { queue: [], lags: [] }
    composer.addEventListener('keydown', (e) => { if (e.key.length === 1) state.queue.push(e.timeStamp) }, true)
    composer.addEventListener('input', () => { const t0 = state.queue.shift(); if (t0 !== undefined) perf.afterPaint().then((t1) => state.lags.push(t1 - t0)) })
    perf.typingState = state
    return true
  }
  // Frame timestamps between start and stop, for scroll frame gaps.
  perf.frames = { list: [], on: false }
  perf.startFrames = () => { perf.frames = { list: [], on: true }; const tick = (t) => { if (!perf.frames.on) return; perf.frames.list.push(t); requestAnimationFrame(tick) }; requestAnimationFrame(tick); return true }
  perf.stopFrames = () => { perf.frames.on = false; return perf.frames.list }
  return perf
})()
true
'''


# ---------------------------------------------------------------------------- fixture, fake provider and launch

def driven_fixture(root, **sizes):
    """The suite fixture with OpenCode on (its executable is the fake) and the long and one short chat pinned."""
    root = suite.write_fixture(root, **sizes)
    providers = ['claude-cli', 'codex-cli', 'gemini-cli', 'opencode-cli', 'copilot-cli', 'antigravity-cli']
    availability = [{'providerId': p, 'executionHostId': 'local', 'enabled': p == PROVIDER} for p in providers]
    (root / 'settings.txt').write_text('\n'.join([
        'settings_format_version=1', f'active_provider_id={PROVIDER}', f'default_new_chat_provider_id={PROVIDER}',
        'remember_last_chat=1', f'last_selected_chat_id={SHORT_CHAT}', 'git_enabled=0', 'svn_enabled=0', 'git_snapshot_plain_folders=0',
        'memory_enabled_default=0', 'memory_level_default=off', 'update_checks_enabled=0',
        f'provider_availability={json.dumps(availability, separators=(",", ":"))}', 'settings_complete=1', '']), encoding='utf-8')
    for chat_id in (LONG_CHAT, SHORT_CHAT):
        path = root / 'chats' / f'{chat_id}.json'
        chat = json.loads(path.read_text(encoding='utf-8'))
        chat.update(pinned=True, provider_id=PROVIDER)
        path.write_text(json.dumps(chat), encoding='utf-8')
    return root


def write_shims(folder):
    """Every provider executable name resolves to the fake ACP peer, so no real CLI or credential is ever used."""
    folder.mkdir(parents=True, exist_ok=True)
    for name in SHIMS:
        if os.name == 'nt':
            (folder / f'{name}.cmd').write_text(f'@"{sys.executable}" "{FAKE}" --fake-provider %*\r\n', encoding='utf-8')
        else:
            shim = folder / name
            shim.write_text(f'#!/bin/sh\nexec "{sys.executable}" "{FAKE}" --fake-provider "$@"\n', encoding='utf-8')
            shim.chmod(0o700)
    return folder


def free_port():
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


def process_names():
    """{pid: (ppid, executable name)} for every process."""
    table = {}
    if os.name == 'nt':
        script = 'Get-CimInstance Win32_Process | ForEach-Object { "$($_.ProcessId)|$($_.ParentProcessId)|$($_.Name)" }'
        output = subprocess.run(['powershell', '-NoProfile', '-Command', script], capture_output=True, text=True).stdout
        rows = (line.split('|', 2) for line in output.splitlines())
    else:
        output = subprocess.run(['ps', '-axo', 'pid=,ppid=,comm='], capture_output=True, text=True).stdout
        rows = (line.split(None, 2) for line in output.splitlines())
    for row in rows:
        if len(row) == 3 and row[0].strip().isdigit() and row[1].strip().isdigit():
            table[int(row[0])] = (int(row[1]), row[2].strip())
    return table


def app_group(root_pid):
    """The app's own processes (main, CEF helpers, its own stdio servers): the tree minus agent subtrees, which on this
    fixture are the fake provider (python) and its shell. Mirrors the reference method's `app` group."""
    table = process_names()
    pids = suite.tree(root_pid, {pid: (ppid, 0) for pid, (ppid, _) in table.items()})
    return {pid for pid in pids if 'universal_agent_manager' in os.path.basename(table[pid][1])}


class Sampler(threading.Thread):
    """Samples the app group's footprint as fast as the platform tools allow (target 4 Hz) until stopped, finding the group
    again every 10 s. While `paused` is set it takes no samples, so the tools it starts do not load the machine during a
    timed probe."""

    def __init__(self, root_pid, period=0.25):
        super().__init__(daemon=True)
        self.root_pid, self.period, self.samples, self.done, self.paused = root_pid, period, [], threading.Event(), threading.Event()

    def run(self):
        group, found = set(), 0.0
        while not self.done.is_set():
            started = time.time()
            if not self.paused.is_set():
                if started - found > 10:
                    group, found = app_group(self.root_pid), started
                self.samples.append(suite.memory_mb(group))
            self.done.wait(max(0.0, self.period - (time.time() - started)))

    def finish(self):
        self.done.set()
        self.join()
        return self.samples


def percentile(values, share):
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * share) - 1)] if ordered else None


class App:
    """The packaged app on a fresh driven fixture with DevTools on a loopback port, plus the fake provider's log."""

    def __init__(self, app, stream_chunks, timeout, keep):
        self.work = Path(tempfile.mkdtemp(prefix='uam-perf-driven-'))
        self.keep, self.process, self.cdp = keep, None, None
        try:
            self._start(app, stream_chunks, timeout)
        except BaseException:  # Ctrl+C too: never leave the app (and its DevTools port) running
            self.snapshot('startup')
            self.close()
            raise

    def _start(self, app, stream_chunks, timeout):
        self.data = driven_fixture(self.work / 'data')
        self.fake_log = self.work / 'fake-provider.jsonl'
        environment = suite.hermetic_environment(self.data)
        environment['PATH'] = str(write_shims(self.work / 'bin')) + os.pathsep + environment['PATH']
        environment.update(UAM_FAKE_PROVIDER_LOG=str(self.fake_log), UAM_FAKE_STREAM=f'{STREAM_CHUNK_MS},{stream_chunks}',
                           UAM_OPENCODE_ZEN_MODELS_PATH=str(self.work / 'zen-models.json'))
        (self.work / 'zen-models.json').write_text('{}')
        self.port = free_port()
        # Chromium's own switches: DevTools on a 127.0.0.1 port, and frames keep coming if another window covers the app,
        # as they do for a visible one. The crash supervisor passes them to the GUI process unchanged.
        arguments = [f'--remote-debugging-port={self.port}', '--disable-backgrounding-occluded-windows', '--disable-renderer-backgrounding']
        self.process = subprocess.Popen([str(suite.app_executable(app)), *arguments], env=environment,
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.cdp = Cdp(self.port, timeout, self.process)
        self.cdp.js(PAGE_HELPERS)
        # Ready when the sidebar and the remembered chat's composer have rendered from the first native state.
        self.cdp.js(f"__perf.until(() => document.querySelector('[data-testid=\"sidebar-tree-scroll\"]') && __perf.composer(), {timeout * 1000}, 'the first chat view')")
        self.viewport = self.cdp.js('[innerWidth, innerHeight, devicePixelRatio]')

    def snapshot(self, label):
        """On a failure: a screenshot and the page text in the run folder (kept with --keep), to see what the app showed."""
        if not self.cdp:
            return
        try:
            (self.work / f'failure-{label}.png').write_bytes(base64.b64decode(self.cdp.call('Page.captureScreenshot')['data']))
            (self.work / f'failure-{label}.txt').write_text(self.cdp.js('document.body.innerText.slice(0, 4000)') or '', encoding='utf-8')
        except Exception:  # the page may be gone; the logs in the data root remain
            pass

    def prompts(self):
        if not self.fake_log.exists():
            return []
        entries = []
        for line in self.fake_log.read_text(errors='replace').splitlines():
            try:
                entries.append(json.loads(line))
            except ValueError:  # several fake processes append to one file; a torn line is skipped
                pass
        return [entry for entry in entries if entry.get('method') == 'session/prompt']

    def open_chat(self, chat_id):
        self.cdp.click(f'[data-testid="pinned-chats"] [data-session-id="{chat_id}"]')
        self.cdp.js(f"__perf.until(() => document.querySelector('[aria-current=\"page\"][data-session-id=\"{chat_id}\"]') && __perf.composer(), 30000, 'chat {chat_id}')")

    def create_chat(self):
        """New chat with the defaults: (ms from the New chat click to an enabled Create, ms from Create to a usable composer)."""
        self.cdp.click('button[title^="New chat ("]')
        dialog = self.cdp.js("__perf.until(() => __perf.create() && !__perf.create().disabled, 30000, 'an enabled Create chat')"
                             ".then(() => __perf.afterPaint()).then((t) => t - __perf.down)")
        # Armed before the click, so a composer that appears while the click is still being acknowledged is not missed.
        self.cdp.js("window.__perfWait = __perf.newComposer(__perf.composer()); __perf.create().setAttribute('data-perf-create', ''); true")
        self.cdp.click('[data-perf-create]')
        return dialog, self.cdp.js('window.__perfWait')

    def send(self, text):
        """Types the prompt into the focused chat's composer and presses Enter; the Enter keydown is the trigger."""
        self.cdp.click('textarea.uam-composer-textarea')
        self.cdp.call('Input.insertText', text=text)
        self.cdp.key(enter=True)

    def wait_idle(self, timeout=60):
        self.cdp.js(f"__perf.until(() => __perf.assistantWith('Streaming') && !__perf.streaming(), {timeout * 1000}, 'the reply to finish')")

    def close(self):
        if self.cdp:
            self.cdp.close()
        if self.process:
            suite.stop(self.process)
            if sys.platform == 'darwin':
                subprocess.run([sys.executable, str(suite.ROOT / 'scripts' / 'prune_code_sign_clones.py')], capture_output=True)
        if self.keep:
            print(f'Kept the run folder: {self.work}')
            return
        for _ in range(10):  # Windows can hold files of a just-stopped process for a moment
            shutil.rmtree(self.work, ignore_errors=True)
            if not self.work.exists():
                break
            time.sleep(1)


# ---------------------------------------------------------------------------- scenarios (one fresh app each)

def scenario_new_thread(app, options):
    """From the open seeded short chat: New chat, then Create; Create's click to a usable composer in the new chat."""
    dialogs, ready = [], []
    for _ in range(options.samples):
        app.open_chat(SHORT_CHAT)
        dialog, ms = app.create_chat()
        dialogs.append(dialog)
        ready.append(ms)
    return {'new_thread_ms': statistics.median(ready), 'new_thread_dialog_ms': statistics.median(dialogs)}


def scenario_first_word(app, options):
    """In a new empty chat: the Enter that sends a prompt to the first streamed word painted (the fake answers at once)."""
    words, requests = [], []
    for _ in range(options.samples):
        app.create_chat()
        app.cdp.js("window.__perfWait = __perf.textPainted(() => __perf.assistantWith('Streaming'), 'the first word'); true")
        app.send('stream check')
        words.append(app.cdp.js('window.__perfWait'))
        enter_wall = app.cdp.js('performance.timeOrigin + __perf.down') / 1000
        prompt = app.prompts()[-1]
        requests.append((prompt['time'] - enter_wall) * 1000)
        app.wait_idle()
    return {'first_word_ms': statistics.median(words), 'first_word_request_ms': statistics.median(requests)}


def scenario_long_thread(app, options):
    """From the short chat to the 2,000-message chat until its newest page is painted (UAM opens it at the start of that
    50-message page), then 20 wheel events at 60 Hz toward the top, counting frames over two frame intervals (33 ms at
    60 Hz) while they arrive."""
    opens, gaps, worst, frame, refresh = [], [], [], [], []
    marker = json.dumps(LONG_PAGE)
    for _ in range(min(options.samples, 5)):
        app.open_chat(SHORT_CHAT)
        time.sleep(1)
        app.cdp.js(f"window.__perfWait = __perf.textPainted(() => document.querySelector('.uam-chat-transcript')?.innerText.includes({marker}),"
                   f" 'the newest page of the long chat'); true")
        app.cdp.click(f'[data-testid="pinned-chats"] [data-session-id="{LONG_CHAT}"]')
        opens.append(app.cdp.js('window.__perfWait'))
        time.sleep(1)
        x, y = app.cdp.js("__perf.centre('.uam-chat-transcript')")
        app.cdp.js('__perf.startFrames()')
        time.sleep(1)  # idle frames first: the display's (or RDP session's) own frame interval
        idle = app.cdp.js('__perf.frames.list.slice()')
        start = app.cdp.js('performance.now()')
        next_at = time.perf_counter()
        for _ in range(20):
            app.cdp.call('Input.dispatchMouseEvent', type='mouseWheel', x=x, y=y, deltaX=0, deltaY=-120)
            next_at += 1 / 60
            time.sleep(max(0.0, next_at - time.perf_counter()))
        end = app.cdp.js('performance.now()')
        frames = [t for t in app.cdp.js('__perf.stopFrames()') if start <= t <= end]
        intervals = [b - a for a, b in zip(frames, frames[1:])]
        # A gap is a frame that took over two idle frame intervals: 33 ms at 60 Hz (the reference), 67 ms where the display or
        # an RDP session runs at 30 frames per second. The idle interval is measured before scrolling, so a slow scroll still counts.
        idle_intervals = [b - a for a, b in zip(idle, idle[1:])]
        refresh.append(statistics.median(idle_intervals) if idle_intervals else 1000 / 60)
        gaps.append(sum(1 for interval in intervals if interval > 2 * refresh[-1] + 0.1))
        worst.append(max(intervals, default=end - start))
        frame.extend(intervals)
    return {'long_thread_ms': statistics.median(opens), 'long_thread_scroll_gaps': statistics.median(gaps),
            'long_thread_scroll_max_gap_ms': statistics.median(worst), 'long_thread_frame_ms': statistics.median(frame) if frame else None,
            'long_thread_refresh_ms': statistics.median(refresh)}


def scenario_typing(app, options):
    """Five new chats each streaming at about 80 tokens/s, started 2 s apart, the last kept in front. The app group's footprint
    is sampled for the load window (p95 and peak), and 60 letters are typed into the front chat's composer, one every 250 ms
    ±20 %, each timed from its keydown to the frame after its echo; no echo within 2 s is a miss."""
    for index in range(5):
        app.create_chat()
        app.send('stream check')
        app.cdp.js("__perf.until(() => __perf.assistantWith('Streaming'), 30000, 'the stream to start')")
        if index < 4:
            time.sleep(2)
    if len({entry['pid'] for entry in app.prompts()}) < 5:
        raise RuntimeError('Fewer than five fake agents received a prompt.')
    sampler = Sampler(app.process.pid)
    window_end = time.time() + options.load
    sampler.start()
    time.sleep(3)
    app.cdp.click('textarea.uam-composer-textarea')
    app.cdp.js('__perf.typing(__perf.composer())')
    sampler.paused.set()
    letters = 'abcdefghijklmnopqrstuvwxyz'
    for index in range(60):
        app.cdp.key(letters[index % len(letters)])
        time.sleep(0.25 * random.uniform(0.8, 1.2))
    time.sleep(2)
    state = app.cdp.js('({lags: __perf.typingState.lags, queued: __perf.typingState.queue.length, streaming: __perf.streaming()})')
    sampler.paused.clear()
    time.sleep(max(0.0, window_end - time.time()))
    still_streaming = app.cdp.js('__perf.streaming()')
    samples = sampler.finish()
    if not still_streaming:
        print('Warning: the front chat had stopped streaming before the load window ended; raise the stream length.', file=sys.stderr)
    lags = [lag for lag in state['lags'] if lag <= 2000]
    return {'typing_lag_p95_ms': percentile(lags, 0.95), 'typing_lag_p50_ms': statistics.median(lags) if lags else None,
            'typing_lag_max_ms': max(lags, default=None), 'typing_missed': 60 - len(lags),
            'ram_under_load_mb': percentile(samples, 0.95), 'ram_under_load_peak_mb': max(samples, default=None),
            'ram_under_load_samples': len(samples)}


def scenario_per_thread(app, options):
    """App-group footprint at rest with 5 and then 20 chats, each with a live fake session that has answered one prompt:
    (median at 20 − median at 5) ÷ 15, each median over the rest window."""
    def add_chats(count):
        for _ in range(count):
            app.create_chat()
            app.send('stream check')
            app.wait_idle()

    def rest():
        sampler = Sampler(app.process.pid)
        sampler.start()
        time.sleep(options.rest)
        return statistics.median(sampler.finish())

    add_chats(5)
    at_five = rest()
    add_chats(15)
    at_twenty = rest()
    table = process_names()
    live = {entry['pid'] for entry in app.prompts()} & set(table)
    return {'per_thread_mb': (at_twenty - at_five) / 15, 'ram_rest_5_threads_mb': at_five, 'ram_rest_20_threads_mb': at_twenty,
            'per_thread_live_agents': len(live)}


SCENARIO_RUNS = {'new_thread': (scenario_new_thread, 8), 'first_word': (scenario_first_word, 8), 'long_thread': (scenario_long_thread, 8),
                 'typing': (scenario_typing, None), 'per_thread': (scenario_per_thread, 8)}


def run_driven(app, out, options):
    """Each selected scenario on `runs` fresh launches; a metric is the median of the per-run values (all kept under raw)."""
    if not suite.app_executable(app).exists():
        raise SystemExit(f'No app executable at {suite.app_executable(app)}.')
    info = suite.desktop()
    if not info['interactive']:
        print('Warning: no interactive desktop (Windows session 0 or no GUI login); CEF window, GPU and paint timings will not '
              'represent a user\'s machine.', file=sys.stderr)
    failures = 0
    for name in options.only or SCENARIOS:
        function, chunks = SCENARIO_RUNS[name]
        # The typing streams must outlast the five chats' setup and the whole load window.
        chunks = chunks or int((options.load + 120) * 1000 / STREAM_CHUNK_MS)
        per_run, sessions, viewport, run, attempts = {}, [], None, 0, 0
        while run < options.runs and attempts < options.runs * 2:
            attempts += 1
            app_run, record = None, {}
            try:
                record = dict(suite.wait_for_desktop())
                app_run = App(app, chunks, options.timeout, options.keep)
                viewport = app_run.viewport
                record.update(suite.gpu_state(app_group(app_run.process.pid)))
                values = function(app_run, options)
                ended = suite.desktop()
                record['state_at_end'] = ended.get('session_state')
                if record['interactive'] and not ended['interactive']:
                    record['outcome'] = 'discarded: the session stopped being Active'
                    print(f'{name} run {run + 1} discarded: the session stopped being Active during it.', file=sys.stderr)
                    continue
                for key, value in values.items():
                    per_run.setdefault(key, []).append(None if value is None else round(value, 2))
                record['outcome'] = 'ok'
                run += 1
            except Exception as error:  # one failed run keeps the others
                if app_run:
                    app_run.snapshot(name)
                failures += 1
                run += 1
                record['outcome'] = f'failed: {error}'
                print(f'{name} run {run} failed: {error}', file=sys.stderr)
            finally:
                sessions.append(record)
                if app_run:
                    app_run.close()
        metrics = {key: round(statistics.median([v for v in values if v is not None]), 2) for key, values in per_run.items()
                   if any(v is not None for v in values)}
        if not metrics and out and out.exists():
            # No run succeeded: clear this scenario's earlier figures rather than report them under the new revision.
            earlier = json.loads(out.read_text()).get('raw', {}).get(f'driven_{name}', {})
            metrics = {key: None for key in earlier if key != 'runs'}
        suite.merge_out(out, metrics, raw={f'driven_{name}': {**per_run, 'runs': sessions}},
                        meta_extra={'driven': {**info, 'viewport': viewport, 'runs': options.runs, 'samples': options.samples,
                                               'load_window_s': options.load, 'rest_window_s': options.rest}})
    return 1 if failures else 0
