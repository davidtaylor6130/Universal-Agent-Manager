#!/usr/bin/env python3
"""Start the companion-enabled Mac preview without replacing the daily UAM app."""
import argparse
import ipaddress
import shutil
import os
from pathlib import Path
import re
import secrets
import subprocess
from urllib.parse import urlsplit


def stop_proxy(proxy):
    proxy.terminate()
    try:
        proxy.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proxy.kill()
        proxy.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--desktop', type=Path, default=Path(__file__).resolve().parents[1] /
                        'Builds/react-companion-preview/universal_agent_manager-4.9.0-alpha-24-UNRecommended.app')
    parser.add_argument('--origin', help='HTTPS origin of the LAN reverse proxy, without a path')
    parser.add_argument('--lan-host', help='Private IPv4 address of this Mac; enables LAN HTTPS on port 58950')
    args = parser.parse_args()
    if args.lan_host:
        try:
            address = ipaddress.IPv4Address(args.lan_host)
            if not any(address in network for network in (ipaddress.IPv4Network('10.0.0.0/8'), ipaddress.IPv4Network('172.16.0.0/12'), ipaddress.IPv4Network('192.168.0.0/16'))):
                raise ValueError()
        except ValueError:
            parser.error('--lan-host must be a private LAN IPv4 address')
        if args.origin:
            parser.error('Use either --lan-host or --origin')
        if not shutil.which('caddy'):
            parser.error('LAN HTTPS requires Caddy: brew install caddy')
        args.origin = 'https://' + args.lan_host + ':58950'
    if args.origin:
        origin = urlsplit(args.origin)
        if (origin.scheme != 'https' or not origin.hostname or origin.username or origin.password
                or args.origin != 'https://' + origin.netloc):
            parser.error('--origin must be an HTTPS origin without a path')
    executable = args.desktop.resolve() / 'Contents/MacOS/universal_agent_manager'
    if not executable.is_file():
        parser.error('Companion desktop preview is missing: ' + str(executable))
    processes = subprocess.check_output(['ps', '-axo', 'command='], text=True)
    for command in processes.splitlines():
        if re.search(r'/Contents/MacOS/universal_agent_manager(?:\s|$)', command) and ' --uam-' not in command:
            parser.error('Quit the running UAM desktop first. Your existing app will not be stopped or replaced.')
    support = Path.home() / 'Library/Application Support'
    directory = support / 'UAM Companion'
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    token = directory / 'token'
    if not token.exists():
        try:
            descriptor = os.open(token, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        except FileExistsError:
            pass
        else:
            with os.fdopen(descriptor, 'w') as output:
                output.write(secrets.token_hex(32))
    if not re.fullmatch(r'[0-9a-fA-F]{64}', token.read_text().strip()):
        parser.error('Existing companion token is invalid; it was not overwritten.')
    token.chmod(0o600)
    environment = dict(os.environ, UAM_COMPANION_TOKEN_FILE=str(token), UAM_COMPANION_PORT='58948',
                       UAM_DATA_DIR=str(support / 'Universal Agent Manager'))
    if args.origin:
        environment['UAM_COMPANION_ORIGIN'] = args.origin
    print('Open http://127.0.0.1:58948/companion and paste the connection token from ' + str(token), flush=True)
    if not args.lan_host:
        os.execve(executable, [str(executable)], environment)
    proxy_environment = dict(os.environ, UAM_LAN_HOST=args.lan_host, UAM_COMPANION_PORT='58948',
                             XDG_DATA_HOME=str(directory / 'proxy-data'), XDG_CONFIG_HOME=str(directory / 'proxy-config'))
    config = Path(__file__).with_name('Caddyfile')
    subprocess.run(['caddy', 'validate', '--config', str(config)], env=proxy_environment, check=True)
    print('Phone URL: ' + args.origin + '/companion', flush=True)
    print('Trust certificate: ' + str(directory / 'proxy-data/caddy/pki/authorities/local/root.crt'), flush=True)
    with subprocess.Popen(['caddy', 'run', '--config', str(config)], env=proxy_environment) as proxy:
        try:
            with subprocess.Popen([str(executable)], env=environment) as desktop:
                while desktop.poll() is None:
                    if proxy.poll() is not None:
                        print('LAN HTTPS stopped. Desktop UAM remains running; check the Caddy error above.', flush=True)
                        desktop.wait()
                        break
                    try:
                        desktop.wait(timeout=1)
                    except subprocess.TimeoutExpired:
                        pass
        finally:
            stop_proxy(proxy)


if __name__ == '__main__':
    main()
