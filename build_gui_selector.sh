#!/bin/bash
# build_gui_selector.sh
# macOS GUI launcher for the Gemini CLI release-slice build.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

CONFIRM=$(osascript -e '
display dialog "Build Universal Agent Manager for the Gemini CLI release slice only?" buttons {"Cancel", "Build"} default button 2 with title "UAM Gemini CLI Build" with icon note
button returned of result
')

if [ "$CONFIRM" != "Build" ]; then
    echo "Build cancelled."
    exit 0
fi

out_dir="Builds/GeminiCLI"

echo ""
echo "Configuring: GeminiCLI"
echo "Output:      ${out_dir}"
echo ""

cmake -S . -B "${out_dir}" \
    -DUAM_FETCH_DEPS=ON \
    -DUAM_BUILD_TESTS=OFF

echo ""
echo "Building..."
echo ""

cmake --build "${out_dir}" -j8

bundle="${out_dir}/universal_agent_manager.app"
binary="${bundle}/Contents/MacOS/universal_agent_manager"

osascript -e "
display dialog \"Build complete!\" & return & return & \"Configuration: GeminiCLI\" & return & \"Bundle: ${bundle}\" & return & \"Launch: open ${bundle}\" buttons {\"OK\"} default button 1 with title \"UAM Build Complete\" with icon note
"

echo ""
echo "Build complete: GeminiCLI"
echo "Bundle: ${bundle}"
echo "Binary: ${binary}"
echo "Launch: open ${bundle}"
echo ""
