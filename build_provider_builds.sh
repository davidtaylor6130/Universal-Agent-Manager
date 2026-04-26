#!/bin/bash
# build_provider_builds.sh
# Builds the Gemini-only binary.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

out_dir="Builds/GeminiCLI"

echo ""
echo "=========================================="
echo "Building: Gemini-only app"
echo "Output:   ${out_dir}"
echo "=========================================="

npm --prefix UI-V2 ci

cmake -S . -B "${out_dir}" \
    -DUAM_BUILD_TESTS=OFF \
    -DUAM_FETCHCONTENT_BASE_DIR=Builds/_deps \
    -DUAM_ENABLE_RUNTIME_GEMINI_CLI=ON \
    -DUAM_ENABLE_RUNTIME_CODEX_CLI=OFF \
    -DUAM_ENABLE_RUNTIME_CLAUDE_CLI=OFF

cmake --build "${out_dir}" --config Release -j8

echo ""
echo "=========================================="
echo "Gemini-only build complete."
echo "Output: ${out_dir}"
echo "=========================================="
echo ""
