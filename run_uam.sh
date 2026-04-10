#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

bundle_candidates=(
    "Builds/universal_agent_manager.app"
    "Builds/app/universal_agent_manager.app"
)

if [ -d "Builds" ]; then
    while IFS= read -r bundle; do
        bundle_candidates+=("$bundle")
    done < <(find Builds -mindepth 2 -maxdepth 2 -name "universal_agent_manager.app" | sort)
fi

for bundle in "${bundle_candidates[@]}"; do
    if [ -d "$bundle" ]; then
        echo "Launching $bundle"
        open "$bundle"
        exit 0
    fi
done

echo "No macOS UAM app bundle was found."
echo "Build the desktop app first with:"
echo "  cmake -S . -B Builds -DUAM_FETCH_DEPS=ON"
echo "  cmake --build Builds --config Release"
exit 1
