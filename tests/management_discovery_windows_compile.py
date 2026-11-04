"""Compile Windows native discovery and its actual GUI-subsystem output helper.

Usage: python3 tests/management_discovery_windows_compile.py [x86_64-w64-mingw32-g++]
This is compile/link evidence only; no Windows executable is run.
"""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

source = Path(__file__).resolve().parent.parent
compiler = shutil.which(sys.argv[1] if len(sys.argv) > 1 else 'x86_64-w64-mingw32-g++')
if not compiler:
    raise SystemExit('Windows cross compiler not found; pass its installed path.')
flags = ['-std=c++20', '-D_WIN32_WINNT=0x0A00', '-Wall', '-Wextra', '-Werror']
with tempfile.TemporaryDirectory(prefix='uam-windows-discovery-compile-') as temporary:
    directory = Path(temporary)
    subprocess.run([compiler, *flags, '-I' + str(source / 'src'),
                    str(source / 'tests/management_discovery_tests.cpp'),
                    str(source / 'src/common/discovery/management_discovery.cpp'),
                    '-ldnsapi', '-o', str(directory / 'native-discovery.exe')], check=True)
    main = (source / 'src/main.cpp').read_text()
    start = main.index('bool WriteDiscoveryJson(')
    end = main.index('std::optional<int> DiscoveryCommand(', start)
    helper = directory / 'discovery-output.cpp'
    helper.write_text('#include <windows.h>\n#include <cstdio>\n#include <string>\n' + main[start:end] +
        '\nint WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)\n'
        '{ return WriteDiscoveryJson("{}\\n") ? 0 : 1; }\n')
    subprocess.run([compiler, *flags, '-municode', '-mwindows', str(helper),
                    '-o', str(directory / 'gui-discovery-output.exe')], check=True)
print('PASS Windows native DNS-SD and exact GUI output helper compile/link; no Windows runtime test.')
