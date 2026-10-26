#!/usr/bin/env python3
"""Exercise actual Windows bootstrap commands with injected helpers and no SSH.

Only the small string-expression grammar used by runner_bootstrap.cpp is accepted.
No C++ is evaluated; unknown syntax is an error. PowerShell fixtures inject helper
commands, while file hashes, snapshots and .NET replacement use temporary files.
"""
import argparse
import ast
import base64
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/remote/runner_bootstrap.cpp"


def compact_powershell(source):
    output = ''
    quote = None
    index = 0
    while index < len(source):
        character = source[index]
        if quote:
            output += character
            if ((character == '`' and quote == '"') or
                    (character == "'" and quote == "'" and index + 1 < len(source) and source[index + 1] == "'")):
                if index + 1 < len(source):
                    index += 1
                    output += source[index]
            elif character == quote:
                quote = None
        elif character in "'\"":
            quote = character
            output += character
        elif character.isspace():
            while index + 1 < len(source) and source[index + 1].isspace():
                index += 1
            if output and index + 1 < len(source) and output[-1] not in '{};' and source[index + 1] not in '{};':
                output += ' '
        else:
            output += character
        index += 1
    return output


def expression_at(source, marker):
    remaining = source[source.index(marker) + len(marker):]
    quoted = False
    escaped = False
    for index, character in enumerate(remaining):
        if quoted:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                quoted = False
        elif character == '"':
            quoted = True
        elif character == ";":
            return remaining[:index]
    raise ValueError("Unterminated source expression: " + marker)


class Strings:
    def __init__(self, source, values):
        self.source = source
        self.values = values

    def evaluate(self, expression):
        tokens = re.findall(r'"(?:\\.|[^"\\])*"|[A-Za-z_][A-Za-z0-9_:>.\-]*|[()+?:,]', expression)
        # Refuse source constructs that the test does not understand.
        assert re.sub(r'\s+', '', ''.join(tokens)) == re.sub(r'\s+', '', expression)
        index = 0

        def primary():
            nonlocal index
            token = tokens[index]
            index += 1
            if token.startswith('"'):
                return ast.literal_eval(token)
            if token == '(':
                value = conditional()
                assert tokens[index] == ')'
                index += 1
                return value
            if index < len(tokens) and tokens[index] == '(':
                index += 1
                args = []
                if tokens[index] != ')':
                    args.append(conditional())
                    while tokens[index] == ',':
                        index += 1
                        args.append(conditional())
                assert tokens[index] == ')'
                index += 1
                if token == 'PowerShellUtf8Command':
                    return compact_powershell(str(args[0]))
                if token in ('PowerShellCommand', 'std::to_string'):
                    return str(args[0])
                if token == 'WindowsRunnerOwnedScript':
                    values = dict(self.values, root=args[0], nonce=args[1], script=args[2])
                    scope = self.source.split('std::string WindowsRunnerOwnedScript(', 1)[1]
                    return Strings(self.source, values).evaluate(expression_at(scope, 'return '))
                assert not args and token.startswith('WindowsRunner'), token
                scope = self.source.split('std::string ' + token + '()', 1)[1]
                return self.evaluate(expression_at(scope, 'return '))
            return self.values[token]

        def concatenated():
            nonlocal index
            value = primary()
            while index < len(tokens) and (tokens[index] == '+' or tokens[index].startswith('"')):
                if tokens[index] == '+':
                    index += 1
                value += primary()
            return value

        def conditional():
            nonlocal index
            value = concatenated()
            if index < len(tokens) and tokens[index] == '?':
                index += 1
                yes = concatenated()
                assert tokens[index] == ':'
                index += 1
                no = conditional()
                value = yes if value else no
            return value

        result = conditional()
        assert index == len(tokens), tokens[index:]
        return result


def scripts_for(version='4.5.7', nonce='metadata', root='r'):
    source = SOURCE.read_text()
    values = {'plan.version': version, 'plan.nonce': nonce, 'root': root,
              'relative': root + '/' + version, 'artifact->sha256': 'a' * 64,
              'artifact->source_fingerprint': 'b' * 64}
    # The fixture's protocol follows the source contract, rather than a hardcoded assumption.
    protocol = re.search(r'kRunnerProtocolVersion = (\d+)', (ROOT / 'src/remote/runner_protocol.h').read_text()).group(1)
    values['kRunnerProtocolVersion'] = protocol
    strings = Strings(source, values)
    final_scope = source.split('static std::string BootstrapFinalizationCommand', 1)[1]
    final_expr = expression_at(final_scope, 'const std::string script =')
    values['keep_new_runner'] = False
    values['rollback'] = strings.evaluate(final_expr)
    rollback = values['rollback']
    values['keep_new_runner'] = True
    commit = strings.evaluate(final_expr)
    lock_scope = source.split('std::vector<std::string> BuildBootstrapLockArgv', 1)[1]
    recovery_scope = source.split('static std::string BootstrapRecoveryScript', 1)[1]
    recovery_scope = recovery_scope[recovery_scope.index('else'): ]
    values['recover'] = strings.evaluate(expression_at(recovery_scope, 'return'))
    lock_scope = lock_scope[lock_scope.index('command = PowerShellUtf8Command'):]
    guard = strings.evaluate(expression_at(lock_scope, 'command ='))
    activation = source.split('BootstrapResult ExecuteBootstrapPlan', 1)[1]
    activation = activation[activation.index('const std::string paths ='):]
    for name in ('paths', 'checksum', 'identity'):
        values[name] = strings.evaluate(expression_at(activation, 'const std::string ' + name + ' ='))
    transaction = strings.evaluate('WindowsRunnerTransactionScript()')
    common = transaction + strings.evaluate('WindowsRunnerLeaseScript()')
    values.update(nonce=nonce, script=transaction + rollback)
    owned_scope = source.split('std::string WindowsRunnerOwnedScript(', 1)[1]
    rollback = strings.evaluate(expression_at(owned_scope, 'return '))
    values['script'] = transaction + commit
    commit = strings.evaluate(expression_at(owned_scope, 'return '))
    return {'common': compact_powershell(common), 'prepare': strings.evaluate(expression_at(activation, 'const std::string prepare =')),
            'activate': strings.evaluate(expression_at(activation, 'const std::string verify =')),
            'publish': strings.evaluate(expression_at(activation, 'const std::string publish =')),
            'rollback': compact_powershell(rollback), 'commit': compact_powershell(commit),
            'recover': compact_powershell(strings.evaluate('WindowsRunnerOwnedScript(root, plan.nonce, WindowsRunnerTransactionScript() + recover)')), 'guard': guard, 'authorize': strings.evaluate(expression_at(source.split('bool RecoverBootstrapInstallation',1)[1], 'const std::string authorize =')), 'protocol': protocol}


def utf8_command_length(script):
    return len('powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass '
               '-Command "& ([scriptblock]::Create([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String(\'' +
               base64.b64encode(script.encode()).decode() + "'))))\"")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--powershell', default=shutil.which('pwsh') or shutil.which('powershell'))
    native_input = parser.add_mutually_exclusive_group()
    native_input.add_argument('--native-script-bundle', type=Path,
                        help='Use scripts captured by the isolated native production-bootstrap target.')
    native_input.add_argument('--native-binary', type=Path,
                              help='Capture scripts directly from the isolated no-process native target.')
    args = parser.parse_args()
    if not args.powershell:
        parser.error('An existing PowerShell installation is required; nothing is installed by this test.')
    scripts = scripts_for()
    if args.native_script_bundle or args.native_binary:
        bundle = json.loads(args.native_script_bundle.read_text() if args.native_script_bundle else
                            subprocess.check_output([str(args.native_binary.resolve()), '--dump-scripts'], text=True, timeout=30))
        native = bundle['windows']
        guard = native['guard']
        common = guard[:guard.index("$root=Join-Path $HOME")]
        recover = native['recover']
        compiled = {name: native[name] for name in ('prepare', 'activate', 'publish', 'rollback', 'commit', 'guard', 'authorize')}
        compiled.update(common=common, recover=recover, protocol=str(bundle['protocol']))
        assert compiled == scripts, 'Source extraction disagrees with compiled production bootstrap commands.'
        scripts = compiled
        print('Using compiled production bootstrap commands; source-expression equivalence PASS.', flush=True)
    maximum = scripts_for('v' * 64, 'n' * 64, 'd' * 240)
    lengths = {name: utf8_command_length(maximum[name]) for name in ('guard', 'prepare', 'activate', 'publish', 'rollback', 'commit', 'authorize', 'recover')}
    for name, length in lengths.items():
        assert length < 8000, (name, length)
    print('Maximum-path Windows command lengths: ' + json.dumps(lengths), flush=True)
    with tempfile.TemporaryDirectory(prefix='uam-bootstrap-metadata-') as directory:
        data = Path(directory) / 'scripts.json'
        data.write_text(json.dumps(scripts))
        subprocess.run([args.powershell, '-NoLogo', '-NoProfile', '-NonInteractive', '-File',
                        str(ROOT / 'scripts/test_bootstrap_metadata.ps1'), '-ScriptsPath', str(data)], check=True, timeout=60)


if __name__ == '__main__':
    main()
