#!/bin/bash
# build_select_provider.sh
# Compatibility launcher for the Gemini-only build.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

out_dir="Builds/GeminiCLI"

echo "============================================"
echo "  UAM Gemini CLI Build"
echo "============================================"
echo ""
echo "This entrypoint builds the Gemini-only slice."
echo "Codex and Claude are disabled for this artifact."
echo ""
read -rp "  Proceed with build? [y]: " confirm
confirm="${confirm:-y}"
if [[ "${confirm,,}" != "y"* ]]; then
    echo "  Cancelled."
    exit 0
fi

npm --prefix UI-V2 ci

cmake -S . -B "${out_dir}" \
    -DUAM_BUILD_TESTS=OFF \
    -DUAM_FETCHCONTENT_BASE_DIR=Builds/_deps \
    -DUAM_ENABLE_RUNTIME_GEMINI_CLI=ON \
    -DUAM_ENABLE_RUNTIME_CODEX_CLI=OFF \
    -DUAM_ENABLE_RUNTIME_CLAUDE_CLI=OFF

cmake --build "${out_dir}" --config Release -j8

echo ""
echo "============================================"
echo "  Build complete: GeminiCLI"
echo "  Bundle: ${out_dir}/universal_agent_manager.app"
echo "  Launch: open ${out_dir}/universal_agent_manager.app"
echo "============================================"
echo ""
