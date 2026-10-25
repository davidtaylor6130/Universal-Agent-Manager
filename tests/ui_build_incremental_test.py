"""Exercise the real UI build graph without downloading frontend or CEF dependencies."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


class UiBuildTests(unittest.TestCase):
    def test_inputs_noop_recovery_and_packaging(self):
        # MSBuild's file tracker ignores %TEMP%, so a fixture there looks out of date on every
        # Visual Studio build; keep it in the repository's ignored build folder instead.
        scratch = ROOT / 'Builds'
        scratch.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='uam build graph ', dir=scratch) as temporary:
            source = Path(temporary)
            ui = source / 'UI-V2'
            (ui / 'src').mkdir(parents=True)
            (ui / 'public').mkdir()
            (ui / 'node_modules').mkdir()
            (ui / 'node_modules/.package-lock.json').write_text('{}')
            (ui / 'index.html').write_text('fixture')
            entry = ui / 'src/main.js'
            entry.write_text('fixture')
            (ui / 'package.json').write_text(json.dumps({'scripts': {'build': 'node build.cjs'}}))
            (ui / 'build.cjs').write_text('''
const fs = require('node:fs');
const out = process.env.VITE_UAM_OUT_DIR;
fs.mkdirSync(out + '/.vite', {recursive: true});
fs.writeFileSync(out + '/index.html', JSON.stringify({
  VITE_UAM_UI_BUILD_ID: process.env.VITE_UAM_UI_BUILD_ID,
  VITE_UAM_SSH: process.env.VITE_UAM_SSH
}));
fs.writeFileSync(out + '/.vite/manifest.json', '{}');
fs.appendFileSync('runs.txt', 'build\\n');
if (fs.existsSync('src/fail')) process.exit(1);
''')
            (source / 'main.cpp').write_text('int main() { return 0; }\n')
            (source / 'runner.cpp').write_text('int main() { return 0; }\n')
            (source / 'assets').mkdir()
            resource = source / 'assets/icon.txt'
            resource.write_text('first')
            (source / 'CMakeLists.txt').write_text(f'''
cmake_minimum_required(VERSION 3.20)
project(ui_build_fixture LANGUAGES CXX)
set(UAM_PRESENTATION_VERSION "fixture" CACHE STRING "")
option(UAM_ENABLE_SSH "" ON)
include("{(ROOT / 'cmake/ui_build.cmake').as_posix()}")
add_executable(uam_runner runner.cpp)
add_executable(universal_agent_manager main.cpp)
add_dependencies(universal_agent_manager uam_ui_v2_build uam_runner)
include("{(ROOT / 'cmake/package_inputs.cmake').as_posix()}")
add_custom_command(TARGET universal_agent_manager POST_BUILD COMMAND "${{CMAKE_COMMAND}}" -E copy
  "${{UAM_UI_DIST_SRC}}/index.html" "${{CMAKE_BINARY_DIR}}/packaged.html")
''')
            build = source / 'Builds'
            def run(*args, success=True):
                result = subprocess.run(['cmake', *map(str, args)], capture_output=True, text=True)
                self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)

            def count():
                return len((ui / 'runs.txt').read_text().splitlines())

            def make():
                run('--build', build, '--config', 'Release', '--parallel', '2')

            def edit(path, content):
                # Makefile generators compare timestamps at filesystem resolution.
                time.sleep(1.05)
                path.write_text(content)
                make()

            run('-S', source, '-B', build, '-DCMAKE_BUILD_TYPE=Release')
            make()
            self.assertEqual(count(), 1)
            packaged = build / 'packaged.html'
            first = packaged.stat().st_mtime_ns
            make()
            run('-S', source, '-B', build)
            make()
            self.assertEqual(count(), 1, 'No-op builds or reconfiguration rebuilt the UI')
            # Visual Studio runs POST_BUILD on every build, so only Make/Ninja skip a no-op repackage.
            if not (build / 'ALL_BUILD.vcxproj').exists():
                self.assertEqual(packaged.stat().st_mtime_ns, first, 'No-op build repackaged the app')
            edit(source / 'main.cpp', 'int main() { return 1; }\n')
            self.assertEqual(count(), 1, 'Native-only edit rebuilt the UI')
            edit(entry, 'changed')
            self.assertEqual(count(), 2)
            self.assertGreater(packaged.stat().st_mtime_ns, first)
            self.assertEqual(packaged.read_bytes(), (build / 'UI-V2/dist/index.html').read_bytes())
            previous_package = packaged.stat().st_mtime_ns
            edit(resource, 'second')
            self.assertEqual(count(), 2, 'Resource-only edit rebuilt the UI')
            self.assertGreater(packaged.stat().st_mtime_ns, previous_package)
            previous_package = packaged.stat().st_mtime_ns
            edit(source / 'runner.cpp', 'int main() { return 2; }\n')
            self.assertEqual(count(), 2, 'Helper-only edit rebuilt the UI')
            self.assertGreater(packaged.stat().st_mtime_ns, previous_package)
            old_input = ui / 'public/old.txt'
            time.sleep(1.05)
            old_input.write_text('old timestamp')
            os.utime(old_input, (1, 1))
            make()
            self.assertEqual(count(), 3, 'Added input with old timestamp was missed')
            time.sleep(1.05)
            old_input.unlink()
            make()
            self.assertEqual(count(), 4, 'Deleted input was missed')
            edit(ui / '.env.production', 'VITE_FIXTURE=1')
            edit(ui / 'node_modules/.package-lock.json', '{"installed":true}')
            self.assertEqual(count(), 6)
            time.sleep(1.05)
            run('-S', source, '-B', build, '-DUAM_ENABLE_SSH=OFF', '-DUAM_PRESENTATION_VERSION=next')
            make()
            environment = json.loads(packaged.read_text())
            self.assertEqual(environment['VITE_UAM_SSH'], 'OFF')
            self.assertTrue(environment['VITE_UAM_UI_BUILD_ID'].startswith('next-'))
            self.assertEqual(count(), 7)
            shutil.rmtree(build / 'UI-V2/dist')
            make()
            self.assertEqual(count(), 8, 'Deleted output was not rebuilt')
            failure = ui / 'src/fail'
            time.sleep(1.05)
            failure.touch()
            run('--build', build, success=False)
            self.assertFalse((build / 'UI-V2/dist/index.html').exists(), 'Failed build remained valid')
            failure.unlink()
            make()
            self.assertEqual(count(), 10)


if __name__ == '__main__':
    unittest.main()
