"""Check generated compile sources and packaged feature artifacts; no app launch."""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('build', type=Path)
parser.add_argument('--config', default='Release')
parser.add_argument('--artifacts', action='store_true')
args = parser.parse_args()
manifest = args.build / f'uam-build-manifest-{args.config}.txt'
if not manifest.exists():
    manifest = args.build / 'uam-build-manifest-.txt'
text = manifest.read_text()
flags = dict(line.split('=', 1) for line in text.splitlines()[:3])
for feature, source in [('COMPUTER_USE', 'computer_use_mcp_server.cpp'), ('MOBILE_COMPANION', 'cef/companion_server.cpp')]:
    assert (source in text) == (flags[feature] == 'ON'), f'{feature}: wrong compiled sources'
if flags['COMPUTER_USE'] == 'OFF':
    assert 'computer_use_disabled.cpp' in text
    assert 'computer_use_platform_macos.mm' not in text and 'computer_use_platform_windows.cpp' not in text
if args.artifacts:
    app = args.build / 'universal_agent_manager.app'
    if app.exists():
        resources = app / 'Contents/Resources'
        assert (app / 'Contents/Frameworks/UAM Computer Use.app').exists() == (flags['COMPUTER_USE'] == 'ON')
    else:
        resources = args.build / args.config
        assert (resources / 'universal_agent_manager.exe').exists()
    assert (resources / 'SoftwareCursor.png').exists() == (flags['COMPUTER_USE'] == 'ON')
    assert (resources / 'remote').exists() == (flags['SSH'] == 'ON')
    companion_chunks = list((resources / 'UI-V2/dist/assets').glob('CompanionShell-*.js'))
    assert bool(companion_chunks) == (flags['MOBILE_COMPANION'] == 'ON'), 'Wrong mobile frontend chunks'
    if flags['MOBILE_COMPANION'] == 'OFF':
        assert not (resources / 'companion').exists()
print('Feature sources' + (' and artifacts' if args.artifacts else '') + ' match: ' + ', '.join(f'{key}={value}' for key, value in flags.items()))
