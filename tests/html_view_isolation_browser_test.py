#!/usr/bin/env python3
"""Headless Chromium test that an agent HTML view card cannot reach the UAM bridge, app data or the network.

It frames the shipped frame host (UI-V2/public/html-view-frame.html) exactly as the card does: the card's sandbox
token from htmlView.ts, the frame host's native policy from html_view.h as its response header, and the page sent
in by postMessage. The embedding page stands in for the UAM window with a fake `cefQuery` bridge, a stored value and
a cookie. An escape page then tries the bridge, the parent document, storage, cookies, navigation, pop-ups and every
network route the policy covers; each attempt reports back by postMessage, and a local listener records any request.

WebRTC and preconnect/dns-prefetch are outside the policy and are removed natively in UAM's renderer; core tests and
docs/chat/html-views.md cover them, so this test does not.

Usage: python3 tests/html_view_isolation_browser_test.py [--browser PATH] [--require] [--control]
Without a Chromium-based browser it reports a skip and exits 0, unless --require is given. --control adds
allow-same-origin to the sandbox and passes only if the test then sees the escapes, proving it can fail.
"""
import argparse
import glob
import http.server
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FRAME_HOST = ROOT / 'UI-V2' / 'public' / 'html-view-frame.html'

ESCAPE_PAGE = '''<!doctype html><body><script>
const report = (name, escaped) => parent.postMessage({ uamProbe: name, escaped: Boolean(escaped) }, '*');
const attempt = (name, run) => { let escaped = false; try { escaped = run() } catch (e) {} report(name, escaped) };
attempt('own-bridge', () => typeof window.cefQuery !== 'undefined' || typeof window.cefQueryCancel !== 'undefined');
attempt('parent-bridge', () => { parent.cefQuery({ request: '{"action":"probe"}' }); return true });
attempt('top-bridge', () => { top.cefQuery({ request: '{"action":"probe"}' }); return true });
attempt('parent-document', () => parent.document.title !== undefined);
attempt('parent-storage', () => parent.localStorage.getItem('uam-secret') !== null);
attempt('local-storage', () => { localStorage.setItem('a', '1'); return true });
attempt('session-storage', () => { sessionStorage.setItem('a', '1'); return true });
attempt('cookie', () => { document.cookie = 'probe=1'; return document.cookie.length > 0 });
attempt('indexed-db', () => { indexedDB.open('probe'); return true });
attempt('popup', () => window.open('__LISTENER__/popup') !== null);
attempt('top-navigation', () => { top.location.href = '__LISTENER__/top'; return true });
attempt('origin', () => location.origin !== 'null' && self.origin !== 'null');
fetch('__LISTENER__/fetch').then(() => report('fetch', true), () => report('fetch', false));
fetch('file:///etc/hosts').then(() => report('file-fetch', true), () => report('file-fetch', false));
fetch('/html-view-frame.html').then(() => report('app-fetch', true), () => report('app-fetch', false));
new Image().src = '__LISTENER__/image';
const style = document.createElement('link'); style.rel = 'stylesheet'; style.href = '__LISTENER__/style'; document.head.append(style);
const script = document.createElement('script'); script.src = '__LISTENER__/script'; document.head.append(script);
const frame = document.createElement('iframe'); frame.src = '__LISTENER__/frame'; document.body.append(frame);
try { new WebSocket('__LISTENER__'.replace('http', 'ws') + '/socket').onopen = () => report('websocket', true) } catch (e) {}
try { new EventSource('__LISTENER__/events').onopen = () => report('event-source', true) } catch (e) {}
try { navigator.sendBeacon('__LISTENER__/beacon') } catch (e) {}
try { new Worker('data:text/javascript,postMessage(1)').onmessage = () => report('worker', true) } catch (e) {}
const form = document.createElement('form'); form.action = '__LISTENER__/form'; form.method = 'post'; document.body.append(form);
try { form.submit() } catch (e) {}
setTimeout(() => report('done', false), 1500);
</script></body>'''

EMBEDDER = '''<!doctype html><html><head><title>UAM</title></head><body><script>
let bridgeCalls = 0;
window.cefQuery = () => { bridgeCalls += 1 };
window.cefQueryCancel = () => { bridgeCalls += 1 };
localStorage.setItem('uam-secret', '1');
document.cookie = 'uam-session=1';
// Canary: the unsandboxed embedder can reach the listener, so "no listener hits" from the page proves something.
new Image().src = '__LISTENER__/canary';
const results = {};
const frame = document.createElement('iframe');
frame.setAttribute('sandbox', __SANDBOX__);
frame.src = '/html-view-frame.html';
// Same handshake as HtmlViewCard: one message with the page text once the host has loaded.
frame.addEventListener('load', () => frame.contentWindow.postMessage({ type: 'uam-html-view', html: __PAGE__ }, '*'), { once: true });
addEventListener('message', (event) => {
  if (event.source !== frame.contentWindow || !event.data || typeof event.data.uamProbe !== 'string') return;
  results[event.data.uamProbe] = event.data.escaped;
  if (event.data.uamProbe === 'done') setTimeout(() => {
    fetch('/result', { method: 'POST', body: JSON.stringify({ results, bridgeCalls, title: document.title, secret: localStorage.getItem('uam-secret'), location: location.pathname }) });
  }, 500);
});
document.body.append(frame);
</script></body></html>'''


def frame_policy():
    """The native frame host policy: the adjacent string literals of uam::html_view::kContentSecurityPolicy."""
    source = (ROOT / 'src' / 'common' / 'chat' / 'html_view.h').read_text()
    match = re.search(r'kContentSecurityPolicy\s*=\s*((?:"[^"]*"\s*)+);', source)
    if not match:
        raise SystemExit('html_view.h no longer declares kContentSecurityPolicy as string literals')
    return ''.join(re.findall(r'"([^"]*)"', match.group(1)))


def card_sandbox():
    source = (ROOT / 'UI-V2' / 'src' / 'components' / 'chat' / 'htmlView.ts').read_text()
    match = re.search(r"HTML_VIEW_SANDBOX\s*=\s*'([^']*)'", source)
    if not match:
        raise SystemExit('htmlView.ts no longer declares HTML_VIEW_SANDBOX')
    return match.group(1)


def find_browser(explicit):
    candidates = [explicit, os.environ.get('UAM_CHROMIUM')]
    candidates += [shutil.which(name) for name in ('google-chrome', 'google-chrome-stable', 'chromium', 'chromium-browser', 'chrome')]
    candidates += ['/Applications/Google Chrome.app/Contents/MacOS/Google Chrome', '/Applications/Chromium.app/Contents/MacOS/Chromium']
    candidates += sorted(glob.glob(os.path.expanduser('~/Library/Caches/ms-playwright/chromium_headless_shell-*/chrome-mac*/headless_shell')), reverse=True)
    candidates += sorted(glob.glob(os.path.expanduser('~/.cache/ms-playwright/chromium_headless_shell-*/chrome-linux*/headless_shell')), reverse=True)
    return next((path for path in candidates if path and os.path.isfile(path) and os.access(path, os.X_OK)), None)


def serve(handler):
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--browser', help='Chromium-based browser executable')
    parser.add_argument('--require', action='store_true', help='fail instead of skipping when no browser is found')
    parser.add_argument('--control', action='store_true', help='weaken the sandbox and expect the escapes to be seen')
    args = parser.parse_args()
    browser = find_browser(args.browser)
    if not browser:
        print('html view isolation: skipped, no Chromium-based browser found (set UAM_CHROMIUM)')
        return 1 if args.require else 0

    listener_hits = []

    class Listener(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def handle_one_request(self):
            try:
                super().handle_one_request()
            except Exception:
                pass

        def do_any(self):
            listener_hits.append(f'{self.command} {self.path}')
            self.send_response(200)
            self.send_header('Content-Type', 'text/plain')
            self.end_headers()
            self.wfile.write(b'ok')

        do_GET = do_POST = do_OPTIONS = do_any

    listener = serve(Listener)
    listener_url = f'http://127.0.0.1:{listener.server_address[1]}'
    page = ESCAPE_PAGE.replace('__LISTENER__', listener_url)
    embedder = EMBEDDER.replace('__LISTENER__', listener_url).replace('__SANDBOX__', json.dumps(card_sandbox() + (' allow-same-origin' if args.control else ''))).replace('__PAGE__', json.dumps(page).replace('</', '<\\/'))
    policy = frame_policy()
    frame_host = FRAME_HOST.read_bytes()
    meta = re.search(r'http-equiv="Content-Security-Policy" content="([^"]*)"', frame_host.decode())
    if not meta or meta.group(1) != policy:
        print('html view isolation: FAILED, the frame host meta policy differs from the native header policy')
        return 1

    reported = {}
    done = threading.Event()

    class App(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            if self.path == '/result' and not done.is_set():
                reported.update(json.loads(self.rfile.read(int(self.headers.get('Content-Length', 0)))))
                done.set()
            self.send_response(204)
            self.end_headers()

        def do_GET(self):
            path = self.path.split('?')[0]
            if path == '/index.html':
                body, headers = embedder.encode(), {}
            elif path == '/html-view-frame.html':
                body, headers = frame_host, {'Content-Security-Policy': policy}
            else:
                self.send_response(404)
                self.end_headers()
                return
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            for name, value in headers.items():
                self.send_header(name, value)
            self.end_headers()
            self.wfile.write(body)

    app = serve(App)
    with tempfile.TemporaryDirectory(prefix='uam-html-view-isolation-') as profile:
        command = [browser, '--headless', '--disable-gpu', '--no-first-run', '--no-default-browser-check', f'--user-data-dir={profile}',
                   '--remote-debugging-port=0', f'http://127.0.0.1:{app.server_address[1]}/index.html']
        if sys.platform.startswith('linux'):
            command.insert(1, '--no-sandbox')  # CI containers lack user namespaces; the iframe sandbox under test is unaffected.
        process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            finished = done.wait(60)
        finally:
            process.kill()
            process.wait()
    app.shutdown()
    listener.shutdown()

    if not finished:
        print('html view isolation: FAILED, the escape page never finished')
        return 1
    outcome = reported
    failures = [f'escaped: {name}' for name, escaped in outcome['results'].items() if escaped]
    expected = {'own-bridge', 'parent-bridge', 'top-bridge', 'parent-document', 'parent-storage', 'local-storage', 'session-storage',
                'cookie', 'indexed-db', 'popup', 'top-navigation', 'origin', 'fetch', 'file-fetch', 'app-fetch', 'done'}
    failures += [f'no report: {name}' for name in sorted(expected - outcome['results'].keys())]
    if outcome['bridgeCalls']:
        failures.append(f"the parent bridge was called {outcome['bridgeCalls']} times")
    if outcome['title'] != 'UAM' or outcome['secret'] != '1' or outcome['location'] != '/index.html':
        failures.append(f'the embedding page changed: {outcome}')
    if 'GET /canary' not in listener_hits:
        failures.append('the listener never saw the embedding page\'s canary request, so network checks prove nothing')
    failures += [f'network request reached the listener: {hit}' for hit in listener_hits if hit != 'GET /canary']
    if args.control:
        seen = {'escaped: parent-bridge', 'escaped: parent-document', 'escaped: parent-storage'} <= set(failures) and outcome['bridgeCalls'] > 0
        print(f"html view isolation control: {'passed, the weakened sandbox was caught' if seen else 'FAILED, escapes went unseen'}")
        return 0 if seen else 1
    if failures:
        print('html view isolation: FAILED\n  ' + '\n  '.join(failures))
        return 1
    print(f"html view isolation: passed, {len(outcome['results']) - 1} escape attempts blocked, no bridge call, no network request ({os.path.basename(browser)})")
    return 0


if __name__ == '__main__':
    sys.exit(main())
