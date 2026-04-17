#!/bin/bash
# build_select_provider.sh
# Compatibility launcher for the Gemini CLI release-slice build.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

out_dir="Builds/GeminiCLI"

echo "============================================"
echo "  UAM Gemini CLI Build"
echo "============================================"
echo ""
echo "This repository now builds the Gemini CLI release slice only."
echo "Unsupported provider variants are intentionally removed."
echo ""
read -rp "  Proceed with build? [y]: " confirm
confirm="${confirm:-y}"
if [[ "${confirm,,}" != "y"* ]]; then
    echo "  Cancelled."
    exit 0
fi

cmake -S . -B "${out_dir}" \
    -DUAM_FETCH_DEPS=ON \
    -DUAM_BUILD_TESTS=OFF

cmake --build "${out_dir}" -j8

echo ""
echo "============================================"
echo "  Build complete: GeminiCLI"
echo "  Bundle: ${out_dir}/universal_agent_manager.app"
echo "  Launch: open ${out_dir}/universal_agent_manager.app"
echo "============================================"
echo ""
