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

cmake_args=(-S . -B Builds -DUAM_BUILD_TESTS=OFF)
for i in 0 1 2 3 4; do
    cmake_args+=("-D${flags[$i]}=${enabled[$i]}")
done

cmake "${cmake_args[@]}"
cmake --build Builds --config Release
