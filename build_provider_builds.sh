#!/bin/bash
# build_provider_builds.sh
# Builds the Gemini CLI release-slice binary.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

out_dir="Builds/GeminiCLI"

echo ""
echo "=========================================="
echo "Building: Gemini CLI release slice"
echo "Output:   ${out_dir}"
echo "=========================================="

cmake -S . -B "${out_dir}" \
    -DUAM_FETCH_DEPS=ON \
    -DUAM_BUILD_TESTS=OFF

cmake --build "${out_dir}" -j8

echo ""
echo "=========================================="
echo "Gemini CLI build complete."
echo "Output: ${out_dir}"
echo "=========================================="
echo ""
