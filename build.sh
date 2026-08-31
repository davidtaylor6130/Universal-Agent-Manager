#!/usr/bin/env bash

set -euo pipefail

cd "$(dirname "$0")"

case "$(uname -s)" in
    Darwin) platform="macOS" ;;
    *) echo "build.sh supports macOS. Use build.ps1 on Windows." >&2; exit 1 ;;
esac

names=("Gemini CLI" "Codex CLI" "Claude Code CLI" "OpenCode CLI" "GitHub Copilot CLI")
flags=(
    UAM_ENABLE_RUNTIME_GEMINI_CLI
    UAM_ENABLE_RUNTIME_CODEX_CLI
    UAM_ENABLE_RUNTIME_CLAUDE_CLI
    UAM_ENABLE_RUNTIME_OPENCODE_CLI
    UAM_ENABLE_RUNTIME_COPILOT_CLI
)
enabled=(ON ON ON ON ON)

while true; do
    echo
    echo "Universal Agent Manager build ($platform)"
    for i in 0 1 2 3 4; do
        printf '  %d) [%s] %s\n' "$((i + 1))" "${enabled[$i]}" "${names[$i]}"
    done
    echo "  B) Build"
    echo "  Q) Quit"
    read -r -p "Toggle a runtime or build: " choice

    case "$choice" in
        [1-5])
            index=$((choice - 1))
            if [[ "${enabled[$index]}" == ON ]]; then
                selected=0
                for state in "${enabled[@]}"; do
                    [[ "$state" == ON ]] && selected=$((selected + 1))
                done
                if [[ "$selected" -eq 1 ]]; then
                    echo "At least one runtime must remain selected."
                else
                    enabled[$index]=OFF
                fi
            else
                enabled[$index]=ON
            fi
            ;;
        [Bb]) break ;;
        [Qq]) exit 0 ;;
        *) echo "Choose 1-5, B, or Q." ;;
    esac
done

npm --prefix UI-V2 ci

command -v zig >/dev/null || { echo "zig is required to build Linux remote helpers." >&2; exit 1; }
command -v x86_64-w64-mingw32-g++ >/dev/null || {
    echo "MinGW-w64 is required to build the Windows remote helper." >&2
    exit 1
}

remote_artifacts="$PWD/Builds/remote-artifacts"
runner_deps="$PWD/Builds/runner-deps"
export ZIG_GLOBAL_CACHE_DIR="${ZIG_GLOBAL_CACHE_DIR:-$PWD/Builds/zig-cache}"

build_remote_runner()
{
    local target="$1"
    local binary="$2"
    shift 2
    local build_dir="$PWD/Builds/runner-$target"
    local artifact_dir="$remote_artifacts/$target"

    cmake -S . -B "$build_dir" \
        -DUAM_RUNNER_ONLY=ON \
        -DCMAKE_BUILD_TYPE=Release \
        -DUAM_FETCHCONTENT_BASE_DIR="$runner_deps" \
        "$@"
    cmake --build "$build_dir" --config Release --target uam_runner --parallel 4
    cmake -E make_directory "$artifact_dir"
    cmake -E copy_if_different "$build_dir/$binary" "$artifact_dir/$binary"
    cmake -E copy_if_different "$build_dir/uam-runner.sha256" "$artifact_dir/uam-runner.sha256"
    cmake -E copy_if_different "$build_dir/uam-runner.version" "$artifact_dir/uam-runner.version"
}

build_remote_runner linux-x86_64 uam-runner \
    -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
    -DCMAKE_C_COMPILER=zig -DCMAKE_C_COMPILER_ARG1=cc \
    -DCMAKE_CXX_COMPILER=zig -DCMAKE_CXX_COMPILER_ARG1=c++ \
    -DCMAKE_C_FLAGS=--target=x86_64-linux-musl \
    -DCMAKE_CXX_FLAGS=--target=x86_64-linux-musl
build_remote_runner linux-arm64 uam-runner \
    -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=arm64 \
    -DCMAKE_C_COMPILER=zig -DCMAKE_C_COMPILER_ARG1=cc \
    -DCMAKE_CXX_COMPILER=zig -DCMAKE_CXX_COMPILER_ARG1=c++ \
    -DCMAKE_C_FLAGS=--target=aarch64-linux-musl \
    -DCMAKE_CXX_FLAGS=--target=aarch64-linux-musl
build_remote_runner windows-x86_64 uam-runner.exe \
    -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
    -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
    -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++

cmake_args=(-S . -B Builds -DUAM_BUILD_TESTS=OFF
    "-DUAM_REMOTE_RUNNER_ARTIFACT_DIR=$remote_artifacts")
for i in 0 1 2 3 4; do
    cmake_args+=("-D${flags[$i]}=${enabled[$i]}")
done

cmake "${cmake_args[@]}"
cmake --build Builds --config Release
