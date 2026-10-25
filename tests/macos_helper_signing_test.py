"""Exercise the production helper generator with cold disposable Mach-O bundles.

Only the generated signing stage runs; no UAM/CEF executable is built or launched.
"""
import argparse
import os
import plistlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
OPTIONS = None
HELPERS = ['universal_agent_manager Helper' + suffix
           for suffix in ['', ' (GPU)', ' (Plugin)', ' (Renderer)']]
JIT_KEYS = ['com.apple.security.cs.allow-jit',
            'com.apple.security.cs.allow-unsigned-executable-memory',
            'com.apple.security.cs.disable-library-validation']


class MacosHelperSigningTests(unittest.TestCase):
    def run_tool(self, *arguments):
        result = subprocess.run(list(map(str, arguments)), capture_output=True, timeout=30,
                                cwd=self.work, env={**os.environ, 'TMPDIR': str(self.work)})
        self.assertEqual(result.returncode, 0,
                         result.stdout.decode(errors='replace') + result.stderr.decode(errors='replace'))
        return result

    def write_plist(self, path, executable, identifier, package='APPL'):
        path.parent.mkdir(parents=True, exist_ok=True)
        values = dict(CFBundleExecutable=executable, CFBundleIdentifier=identifier,
                      CFBundleName=executable, CFBundlePackageType=package,
                      CFBundleVersion='1', CFBundleShortVersionString='1.0')
        for key in ['NSMicrophoneUsageDescription', 'NSSpeechRecognitionUsageDescription',
                    'NSLocalNetworkUsageDescription']:
            values[key] = 'Signing fixture; never launched.'
        path.write_bytes(plistlib.dumps(values))

    def test_cold_and_incremental_nested_signing(self):
        self.assertEqual(sys.platform, 'darwin')
        source = (ROOT / 'CMakeLists.txt').read_text()
        begin = source.index('    set(_cef_plists_dir "${CMAKE_BINARY_DIR}/cef_helper_plists")')
        end = source.index('    # POST_BUILD: run the generated script', begin)
        # Evaluate the real production generator, not a second copy of its ordering.
        generator = source[begin:end]
        for enabled in [True, False]:
            with self.subTest(computer_use=enabled), tempfile.TemporaryDirectory(prefix='uam signing ') as temporary:
                work = Path(temporary)
                self.work = work
                app = work / 'Fixture.app'
                contents = app / 'Contents'
                executable = contents / 'MacOS/universal_agent_manager'
                executable.parent.mkdir(parents=True)
                stub = work / 'stub.c'
                stub.write_text('int main(void) { return 0; }\n')
                arch = ['-arch', OPTIONS.arch] if OPTIONS.arch else []
                self.run_tool(OPTIONS.compiler, *arch, stub, '-o', executable)
                # Remove the linker's implicit arm64 signature too: this is a cold build.
                self.run_tool('/usr/bin/codesign', '--remove-signature', executable)
                self.write_plist(contents / 'Info.plist', executable.name, 'org.uam.signing-fixture')
                framework = contents / 'Frameworks/Chromium Embedded Framework.framework'
                version = framework / 'Versions/A'
                version.mkdir(parents=True)
                self.run_tool(OPTIONS.compiler, *arch, '-dynamiclib', stub, '-o',
                              version / 'Chromium Embedded Framework')
                self.write_plist(version / 'Resources/Info.plist', 'Chromium Embedded Framework',
                                 'org.uam.signing-fixture.cef', 'FMWK')
                (framework / 'Versions/Current').symlink_to('A')
                (framework / 'Resources').symlink_to('Versions/Current/Resources')
                (framework / 'Chromium Embedded Framework').symlink_to('Versions/Current/Chromium Embedded Framework')
                # Intel CEF ships unsigned; strip the linker's arm64 signature to match.
                self.run_tool('/usr/bin/codesign', '--remove-signature', version / 'Chromium Embedded Framework')
                generated = work / 'generated'
                generated.mkdir()
                script = work / 'generate.cmake'
                script.write_text('cmake_minimum_required(VERSION 3.20)\n'
                                  f'set(CMAKE_BINARY_DIR [==[{generated.as_posix()}]==])\n'
                                  f'set(CMAKE_SOURCE_DIR [==[{ROOT.as_posix()}]==])\n'
                                  'set(UAM_MACOS_SIGNING_IDENTITY "-")\n'
                                  f'set(UAM_ENABLE_COMPUTER_USE {"ON" if enabled else "OFF"})\n'
                                  'set(UAM_PRODUCT_VERSION "1.0")\nset(PROJECT_VERSION "1.0")\n' + generator)
                self.run_tool(OPTIONS.cmake, '-P', script)
                for phase in ['cold', 'incremental']:
                    self.run_tool(OPTIONS.cmake, f'-DUAM_BUNDLE_CONTENTS={contents}',
                                  f'-DUAM_BUNDLE_DIR={app}', f'-DUAM_MAIN_EXE={executable}',
                                  '-P', generated / 'cef_helper_plists/create_helpers.cmake')
                    names = HELPERS + (['UAM Computer Use'] if enabled else [])
                    for name in names:
                        helper = contents / f'Frameworks/{name}.app'
                        self.assertTrue(helper.is_dir())
                        # Framework links leave this child but stay inside the enclosing app.
                        # Apply strict path validation at that enclosing boundary below.
                        self.run_tool('/usr/bin/codesign', '--verify', helper)
                        result = self.run_tool('/usr/bin/codesign', '--display', '--entitlements', ':-', helper)
                        entitlements = plistlib.loads(result.stdout) if result.stdout else {}
                        if name == 'UAM Computer Use':
                            self.assertEqual(entitlements, {})
                        else:
                            for key in JIT_KEYS:
                                self.assertIs(entitlements.get(key), True)
                    self.assertEqual((contents / 'Frameworks/UAM Computer Use.app').exists(), enabled)
                    # Match the final app gate: sign explicitly, then deep/strict verify.
                    self.run_tool('/usr/bin/codesign', '--force', '--sign', '-', '--entitlements',
                                  generated / 'cef_helper_plists/jit.entitlements', app)
                    self.run_tool('/usr/bin/codesign', '--verify', '--deep', '--strict', '--verbose=2', app)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cmake', default='cmake')
    parser.add_argument('--compiler', default='/usr/bin/clang')
    parser.add_argument('--arch', choices=['arm64', 'x86_64'])
    OPTIONS, remaining = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0], *remaining])
