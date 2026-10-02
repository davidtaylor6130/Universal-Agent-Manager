#!/usr/bin/env python3
"""Local build UI. Runs fixed build commands and never installs provider CLIs."""
import json
import os
from pathlib import Path
import platform
import secrets
import shutil
import re
import sys
import subprocess
import threading
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = Path(__file__).resolve().parents[2]
PROVIDERS = ('GEMINI', 'CODEX', 'CLAUDE', 'OPENCODE', 'COPILOT')
FEATURES = ('COMPUTER_USE', 'SSH', 'MOBILE_COMPANION')
TOKEN = secrets.token_urlsafe(32)
STATE = {'running': False, 'message': 'Choose providers, features and a build folder.', 'output': '', 'artifact': '', 'dataRoot': '', 'version': ''}
LOCK = threading.Lock()


def configuration(payload):
    if not isinstance(payload.get('source', str(ROOT)), str) or not isinstance(payload.get('build', 'Builds/configured'), str):
        raise ValueError('Source and build folders must be text paths.')
    source = Path(payload.get('source', str(ROOT))).expanduser().resolve()
    if not (source / 'CMakeLists.txt').is_file() or not (source / 'UI-V2/package-lock.json').is_file():
        raise ValueError('Choose a UAM source folder containing CMakeLists.txt and UI-V2/package-lock.json.')
    destination = Path(payload.get('build', 'Builds/configured')).expanduser()
    if not destination.is_absolute():
        destination = source / destination
    destination = destination.resolve()
    if not destination.is_relative_to(source / 'Builds') or destination == source / 'Builds':
        raise ValueError('Choose a separate folder inside the source Builds folder, such as Builds/configured.')
    # Existing build channels require a new folder. Only this tool owns/reuses its own output.
    if destination.exists() and not (destination / '.uam-configurator').is_file() and any(destination.iterdir()):
        raise ValueError('That build folder already contains another build. Choose a new folder inside Builds.')
    if os.name == 'nt' and any(char in str(source) + str(destination) for char in '%!^&|<>\r\n"'):
        raise ValueError('Use a source and build path without Windows command characters (% ! ^ & | < >).')
    providers = payload.get('providers', list(PROVIDERS))
    features = payload.get('features', list(FEATURES))
    if not isinstance(providers, list) or not providers or any(item not in PROVIDERS for item in providers):
        raise ValueError('Choose at least one supported provider.')
    if not isinstance(features, list) or any(item not in FEATURES for item in features):
        raise ValueError('Choose supported features only.')
    return source, destination, providers, features


def msvc_setup():
    if os.name != 'nt' or shutil.which('cl'):
        return None
    finder = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    if finder.is_file():
        result = subprocess.run([str(finder), '-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], capture_output=True, text=True)
        candidate = Path(result.stdout.strip()) / 'VC/Auxiliary/Build/vcvars64.bat'
        if candidate.is_file():
            return candidate
    return None


def prerequisites():
    missing = []
    for command, recovery in [('node', 'Install Node.js LTS from nodejs.org.'), ('npm', 'Install Node.js LTS, which includes npm.'), ('cmake', 'Install CMake from cmake.org and add it to PATH.')]:
        if not shutil.which(command):
            missing.append(recovery)
    if sys.version_info < (3, 9):
        missing.append('Install a current Python 3 release from python.org.')
    for command in ('node', 'cmake'):
        if not shutil.which(command):
            continue
        try:
            result = subprocess.run([command, '--version'], capture_output=True, text=True)
            match = re.search(r'(\d+)\.(\d+)\.(\d+)', result.stdout)
            version = tuple(map(int, match.groups())) if match else (0, 0, 0)
            supported = version >= (3, 20, 0) if command == 'cmake' else ((20, 19, 0) <= version < (21, 0, 0) or version >= (22, 12, 0))
            if result.returncode or not supported:
                missing.append('Install Node.js LTS (20.19+ or 22.12+).' if command == 'node' else 'Install CMake 3.20 or newer.')
        except OSError:
            missing.append(f'{command} could not start. Reinstall it and check again.')
    if platform.system() == 'Darwin' and subprocess.run(['xcode-select', '-p'], capture_output=True).returncode:
        missing.append('Install Apple command line tools: open Terminal and run xcode-select --install.')
    if platform.system() == 'Windows' and not shutil.which('cl') and not msvc_setup():
        missing.append('Install Visual Studio Build Tools with Desktop development with C++, then check again.')
    if platform.system() not in ('Darwin', 'Windows'):
        missing.append('UAM desktop builds support macOS and Windows.')
    return missing


def reserve_local_build_version(source):
    result = subprocess.run(['cmake', f'-DUAM_SOURCE_DIR={source}', '-P', str(ROOT / 'cmake/local_build_version.cmake')], capture_output=True, text=True)
    version = result.stdout.strip()
    if result.returncode or not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+-alpha-[1-9][0-9]*', version):
        raise RuntimeError('Could not reserve a local build version. ' + (result.stderr.strip() or 'Check the shared Builds/local-versions counter.'))
    return version


def commands(source, destination, providers, features, local_version=''):
    npm = shutil.which('npm') or ('npm.cmd' if os.name == 'nt' else 'npm')
    flags = [f'-DUAM_ENABLE_RUNTIME_{item}_CLI={"ON" if item in providers else "OFF"}' for item in PROVIDERS]
    flags += [f'-DUAM_ENABLE_{item}={"ON" if item in features else "OFF"}' for item in FEATURES]
    if local_version:
        flags.append(f'-DUAM_LOCAL_BUILD_VERSION={local_version}')
    return [([npm, '--prefix', str(source / 'UI-V2'), 'ci'], 'Installing frontend dependencies'),
            (['cmake', '-S', str(source), '-B', str(destination), '-DCMAKE_BUILD_TYPE=Release', '-DUAM_PACKAGE_REMOTE_RUNNERS=OFF', *flags], 'Preparing the build'),
            (['cmake', '--build', str(destination), '--config', 'Release', '--parallel', '2', '--target', 'universal_agent_manager'], 'Building UAM')]


def artifact_path(destination):
    return destination / ('universal_agent_manager.app' if os.name != 'nt' else 'Release/universal_agent_manager.exe')


def build(config):
    source, destination, providers, features = config
    try:
        destination.mkdir(parents=True, exist_ok=True)
        (destination / '.uam-configurator').write_text('UAM build configurator output\n')
        local_version = reserve_local_build_version(source)
        with LOCK:
            STATE['version'] = local_version
        log_path = destination / 'configurator-build.log'
        with log_path.open('w', encoding='utf-8') as log:
            log.write(f'Local build version: {local_version}\n')
            for argv, message in commands(*config, local_version=local_version):
                with LOCK:
                    STATE['message'] = f'{message} ({local_version})'
                # Windows .cmd invocation is explicit; options are fixed and validated.
                if os.name == 'nt':
                    setup = msvc_setup()
                    command = ' '.join('\"' + str(arg) + '\"' for arg in argv)
                    if setup:
                        command = f'call \"{setup}\" >nul && ' + command
                    argv = [os.environ.get('COMSPEC', 'cmd.exe'), '/d', '/s', '/c', command]
                process = subprocess.Popen(argv, cwd=source, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
                for line in process.stdout:
                    log.write(line)
                    log.flush()
                    with LOCK:
                        STATE['output'] = (STATE['output'] + line)[-16000:]
                if process.wait():
                    raise RuntimeError(f'{message} failed. Check {log_path} for the error, fix it and build again.')
        artifact = artifact_path(destination)
        if not artifact.exists():
            raise RuntimeError(f'The build finished but {artifact} is missing. Check the build log.')
        with LOCK:
            STATE.update(message=f'Build {local_version} ready. Launch UAM or open its folder.', artifact=str(artifact), dataRoot=str(destination / 'data'))
    except Exception as error:
        with LOCK:
            STATE['message'] = str(error)
    finally:
        with LOCK:
            STATE['running'] = False


def launch_artifact(artifact, data_root):
    environment = os.environ.copy()
    environment['UAM_DATA_DIR'] = data_root
    executable = Path(artifact) if os.name == 'nt' else Path(artifact) / 'Contents/MacOS/universal_agent_manager'
    return subprocess.Popen([str(executable)], cwd=Path(data_root).parent, env=environment)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, status, data, content_type='application/json'):
        body = data.encode() if isinstance(data, str) else json.dumps(data).encode()
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.end_headers()
        self.wfile.write(body)

    def trusted(self):
        expected = f'127.0.0.1:{self.server.server_port}'
        return self.headers.get('Host') == expected and self.headers.get('Origin', f'http://{expected}') == f'http://{expected}'

    def do_GET(self):
        if not self.trusted():
            return self.reply(403, {'error': 'Open the local configurator URL.'})
        if self.path == '/':
            template = Path(__file__).with_name('index.html').read_text()
            return self.reply(200, template.replace('__TOKEN__', TOKEN).replace('__SOURCE__', str(ROOT).replace('&', '&amp;').replace('"', '&quot;')), 'text/html; charset=utf-8')
        if self.path == '/status' and self.headers.get('X-UAM-Token') == TOKEN:
            with LOCK:
                return self.reply(200, STATE.copy())
        self.reply(404, {})

    def do_POST(self):
        if not self.trusted() or self.headers.get('X-UAM-Token') != TOKEN:
            return self.reply(403, {'error': 'Reload the local configurator page.'})
        try:
            size = int(self.headers.get('Content-Length', '0'))
            if not 0 < size <= 8192:
                raise ValueError('Invalid request size.')
            payload = json.loads(self.rfile.read(size))
            if not isinstance(payload, dict):
                raise ValueError('Invalid configuration.')
            if self.path in ('/check', '/build'):
                config = configuration(payload)
                missing = prerequisites()
                if missing:
                    return self.reply(400, {'error': ' '.join(missing)})
                if self.path == '/check':
                    return self.reply(200, {'message': 'Build tools are ready.'})
                with LOCK:
                    if STATE['running']:
                        return self.reply(409, {'error': 'A build is already running.'})
                    STATE.update(running=True, output='', artifact='', dataRoot='', version='', message='Starting build')
                threading.Thread(target=build, args=(config,), daemon=True).start()
                return self.reply(200, {'message': 'Build started.'})
            if self.path in ('/launch', '/folder'):
                with LOCK:
                    artifact = STATE['artifact']
                    data_root = STATE['dataRoot']
                if not artifact or not Path(artifact).exists():
                    raise ValueError('Complete a build first.')
                if self.path == '/launch':
                    launch_artifact(artifact, data_root)
                    return self.reply(200, {'message': 'Opened.'})
                target = str(Path(artifact).parent)
                if os.name == 'nt':
                    os.startfile(target)
                else:
                    subprocess.Popen(['open', target])
                return self.reply(200, {'message': 'Opened.'})
            return self.reply(404, {})
        except (ValueError, OSError) as error:
            self.reply(400, {'error': str(error)})


def main():
    if sys.version_info < (3, 9):
        print('Install a current Python 3 release from python.org, then open the launcher again.')
        return
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    url = f'http://127.0.0.1:{server.server_port}/'
    print(f'Build configurator: {url}\nKeep this window open until the build finishes.', flush=True)
    webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        server.server_close()


if __name__ == '__main__':
    main()
