#!/usr/bin/env bash

set -euo pipefail

archive_path=${1:?Usage: smoke_packaged_macos.sh <archive.zip> [expected-ui-dist]}
expected_ui_dist=${2:-}
smoke_parent=$(cd "${TMPDIR:-/tmp}" && pwd -P)
smoke_root=$(mktemp -d "$smoke_parent/uam-package-smoke.XXXXXX")
app_pid=""

cleanup()
{
    if [[ "$app_pid" =~ ^[0-9]+$ ]] && kill -0 "$app_pid" 2>/dev/null; then
        kill -TERM "$app_pid" 2>/dev/null || true
        for _ in {1..10}; do
            kill -0 "$app_pid" 2>/dev/null || break
            sleep 1
        done
        kill -KILL "$app_pid" 2>/dev/null || true
    fi
    if [[ -n "$smoke_root" && -d "$smoke_root" && "$smoke_root" == "$smoke_parent"/uam-package-smoke.* ]]; then
        rm -rf "$smoke_root"
    fi
}
trap cleanup EXIT INT TERM

unpack_root="$smoke_root/unpacked"
data_root="$smoke_root/data"
mkdir -p "$unpack_root" "$data_root"
ditto -x -k "$archive_path" "$unpack_root"

app_root="$unpack_root/universal_agent_manager.app"
app_exe="$app_root/Contents/MacOS/universal_agent_manager"
packaged_ui="$app_root/Contents/Resources/UI-V2/dist"
cef_framework="$app_root/Contents/Frameworks/Chromium Embedded Framework.framework/Chromium Embedded Framework"
cef_helper="$app_root/Contents/Frameworks/universal_agent_manager Helper.app/Contents/MacOS/universal_agent_manager Helper"
remote_root="$app_root/Contents/Resources/remote"
[[ -x "$app_exe" ]]
[[ -f "$packaged_ui/index.html" ]]
[[ -x "$cef_framework" ]]
[[ -x "$cef_helper" ]]
for remote_file in \
    "$remote_root/uam-runner" \
    "$remote_root/uam-runner.sha256" \
    "$remote_root/uam-runner.version" \
    "$remote_root/linux-arm64/uam-runner" \
    "$remote_root/linux-arm64/uam-runner.sha256" \
    "$remote_root/linux-arm64/uam-runner.version" \
    "$remote_root/linux-x86_64/uam-runner" \
    "$remote_root/linux-x86_64/uam-runner.sha256" \
    "$remote_root/linux-x86_64/uam-runner.version" \
    "$remote_root/windows-x86_64/uam-runner.exe" \
    "$remote_root/windows-x86_64/uam-runner.sha256" \
    "$remote_root/windows-x86_64/uam-runner.version"
do
    [[ -f "$remote_file" ]]
done
for remote_runner in \
    "$remote_root/uam-runner" \
    "$remote_root/linux-arm64/uam-runner" \
    "$remote_root/linux-x86_64/uam-runner" \
    "$remote_root/windows-x86_64/uam-runner.exe"
do
    expected_hash=$(tr -d '[:space:]' < "$(dirname "$remote_runner")/uam-runner.sha256")
    actual_hash=$(shasum -a 256 "$remote_runner" | awk '{print $1}')
    [[ "$actual_hash" == "$expected_hash" ]]
done
native_runner_version=$("$remote_root/uam-runner" --version)
bundle_version=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' \
    "$app_root/Contents/Info.plist")
[[ "$native_runner_version" == "$bundle_version" ]]
for version_file in \
    "$remote_root/uam-runner.version" \
    "$remote_root/linux-arm64/uam-runner.version" \
    "$remote_root/linux-x86_64/uam-runner.version" \
    "$remote_root/windows-x86_64/uam-runner.version"
do
    [[ "$(tr -d '[:space:]' < "$version_file")" == "$native_runner_version" ]]
done
codesign --verify --deep --strict "$app_root"

if [[ -n "$expected_ui_dist" ]]; then
    diff -qr "$expected_ui_dist" "$packaged_ui"
fi

UAM_DATA_DIR="$data_root" /usr/bin/open -n "$app_root"
for _ in {1..10}; do
    app_pid=$(/usr/bin/pgrep -f "$app_exe" || true)
    [[ "$app_pid" =~ ^[0-9]+$ ]] && break
    sleep 1
done

if ! [[ "$app_pid" =~ ^[0-9]+$ ]]; then
    echo "Packaged macOS app did not start through Launch Services." >&2
    exit 1
fi
sleep 8

if ! kill -0 "$app_pid" 2>/dev/null; then
    echo "Packaged macOS app exited during startup." >&2
    exit 1
fi

kill -TERM "$app_pid"
for _ in {1..10}; do
    kill -0 "$app_pid" 2>/dev/null || break
    sleep 1
done
if kill -0 "$app_pid" 2>/dev/null; then
    echo "Packaged macOS app did not stop after SIGTERM." >&2
    exit 1
fi
app_pid=""

echo "Packaged macOS smoke test passed."
