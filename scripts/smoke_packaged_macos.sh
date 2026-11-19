#!/usr/bin/env bash

set -euo pipefail

archive_path=${1:?Usage: smoke_packaged_macos.sh <archive.zip> [expected-ui-dist]}
expected_ui_dist=${2:-}
smoke_parent=$(cd "${TMPDIR:-/tmp}" && pwd -P)
smoke_root=$(mktemp -d "$smoke_parent/uam-package-smoke.XXXXXX")
app_pid=""
gui_pid=""
prune_clones="$(cd "$(dirname "$0")" && pwd -P)/prune_code_sign_clones.py"

cleanup()
{
    for pid in "$app_pid" "$gui_pid"; do
        if [[ "$pid" =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null || true
            for _ in {1..10}; do
                kill -0 "$pid" 2>/dev/null || break
                sleep 1
            done
            kill -KILL "$pid" 2>/dev/null || true
        fi
    done
    # macOS keeps a ~0.5 GB code_sign_clone copy of each launched bundle (#684).
    python3 "$prune_clones" || true
    if [[ -n "$smoke_root" && -d "$smoke_root" && "$smoke_root" == "$smoke_parent"/uam-package-smoke.* ]]; then
        rm -rf "$smoke_root"
    fi
}
trap cleanup EXIT INT TERM
python3 "$prune_clones" || true

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
# The sandboxed helpers load the framework at runtime; a linked framework would load before the sandbox.
helper_links=$(otool -L "$cef_helper")
if [[ "$helper_links" == *"Chromium Embedded Framework"* ]]; then
    echo "CEF helper links the CEF framework instead of loading it after entering the sandbox." >&2
    exit 1
fi
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
# Gemini Plan and goal-review launches refuse to start without UAM's policy beside the app resources.
gemini_policy="$app_root/Contents/Resources/gemini-plan-ceiling.toml"
if ! cmp -s "$gemini_policy" "$(cd "$(dirname "$0")/.." && pwd -P)/assets/providers/gemini-plan-ceiling.toml"; then
    echo "Packaged macOS app is missing the current Gemini Plan policy: $gemini_policy" >&2
    exit 1
fi
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
# The companion transport must be the pinned, hash-checked Caddy release from CMakeLists.txt.
pinned_caddy=$(sed -n 's/^ *set(UAM_CADDY_VERSION "\(.*\)")$/\1/p' "$(dirname "$prune_clones")/../CMakeLists.txt")
packaged_caddy=$("$app_root/Contents/Resources/companion/caddy" version)
if [[ -z "$pinned_caddy" || "$packaged_caddy" != "v$pinned_caddy "* ]]; then
    echo "Packaged Caddy '$packaged_caddy' is not the pinned v$pinned_caddy." >&2
    exit 1
fi

if [[ -n "$expected_ui_dist" ]]; then
    diff -qr "$expected_ui_dist" "$packaged_ui"
fi

/usr/bin/open -n --env "UAM_DATA_DIR=$data_root" "$app_root"
for _ in {1..10}; do
    app_pid=$(/usr/bin/pgrep -o -f "$app_exe" || true)
    [[ "$app_pid" =~ ^[0-9]+$ ]] && break
    sleep 1
done

if ! [[ "$app_pid" =~ ^[0-9]+$ ]]; then
    echo "Packaged macOS app did not start through Launch Services." >&2
    exit 1
fi
for _ in {1..10}; do
    gui_pid=$(/usr/bin/pgrep -P "$app_pid" -f 'uam-supervised-gui' || true)
    [[ "$gui_pid" =~ ^[0-9]+$ ]] && break
    sleep 1
done
if ! [[ "$gui_pid" =~ ^[0-9]+$ ]]; then
    echo "Packaged macOS app supervisor did not launch the GUI." >&2
    exit 1
fi
# The UI logs this after its first native bridge reply, so it proves the renderer ran and the bridge answered.
bridge_marker="[frontend] [UI startup] Initial state loaded through the native bridge."
for _ in {1..60}; do
    grep -qF "$bridge_marker" "$data_root/uam.log" 2>/dev/null && break
    kill -0 "$gui_pid" 2>/dev/null || break
    sleep 1
done
if ! kill -0 "$app_pid" 2>/dev/null || ! kill -0 "$gui_pid" 2>/dev/null; then
    echo "Packaged macOS app or GUI exited during startup." >&2
    exit 1
fi
if ! grep -qF "$bridge_marker" "$data_root/uam.log" 2>/dev/null; then
    echo "Packaged macOS UI did not complete a native bridge round-trip." >&2
    exit 1
fi
renderer_args=$(/bin/ps -axww -o args= | grep -F "$app_root/Contents/Frameworks/" | grep -F -- "--type=renderer" || true)
if [[ -z "$renderer_args" || "$renderer_args" != *"--seatbelt-client="* || "$renderer_args" == *"--no-sandbox"* ]]; then
    echo "Packaged macOS renderer is not running inside the Chromium sandbox." >&2
    exit 1
fi

kill -TERM "$app_pid"
for _ in {1..10}; do
    if ! kill -0 "$app_pid" 2>/dev/null && ! kill -0 "$gui_pid" 2>/dev/null; then
        break
    fi
    sleep 1
done
if kill -0 "$app_pid" 2>/dev/null || kill -0 "$gui_pid" 2>/dev/null; then
    echo "Packaged macOS app did not stop after SIGTERM." >&2
    exit 1
fi
app_pid=""
gui_pid=""

echo "Packaged macOS smoke test passed."
