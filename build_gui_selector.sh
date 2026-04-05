#!/bin/bash
# build_gui_selector.sh
# macOS GUI selector for choosing which providers to enable in the build.
# Uses native AppleScript dialogs — no dependencies required.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

FETCH_DEPS="ON"
FETCH_LLAMA="OFF"
BUILD_TESTS="OFF"

LLAMA_SRC=""
if [ -d "Builds/_deps/llama_cpp_uam-src" ]; then
    LLAMA_SRC="$(pwd)/Builds/_deps/llama_cpp_uam-src"
fi

# ── Step 1: Provider selection dialog ──────────────────────────────────
SELECTION=$(osascript <<'APPLESCRIPT'
set providers to {¬
    "Gemini (Structured)", ¬
    "Gemini (CLI)", ¬
    "Claude Code", ¬
    "Codex CLI", ¬
    "OpenCode CLI", ¬
    "OpenCode Local", ¬
    "Ollama Engine (Local)" ¬
}

set defaults to {true, true, true, true, true, true, true}

set chosen to choose from list providers ¬
    with title "UAM Provider Build Selector" ¬
    with prompt "Select which providers to include in this build:" ¬
    with multiple selections allowed ¬
    default items defaults ¬
    OK button name "Build" ¬
    cancel button name "Cancel"

if chosen is false then
    return "CANCELLED"
end if

set AppleScript's text item delimiters to ","
return chosen as text
APPLESCRIPT
)

if [ "$SELECTION" = "CANCELLED" ] || [ -z "$SELECTION" ]; then
    echo "Build cancelled."
    exit 0
fi

# ── Step 2: Parse selection → flags ────────────────────────────────────
G_S="OFF"; G_C="OFF"; CL="OFF"; CX="OFF"; OC="OFF"; OL="OFF"; OE="OFF"

IFS=',' read -ra ITEMS <<< "$SELECTION"
for item in "${ITEMS[@]}"; do
    item=$(echo "$item" | xargs)
    case "$item" in
        "Gemini (Structured)")  G_S="ON" ;;
        "Gemini (CLI)")         G_C="ON" ;;
        "Claude Code")          CL="ON" ;;
        "Codex CLI")            CX="ON" ;;
        "OpenCode CLI")         OC="ON" ;;
        "OpenCode Local")       OL="ON" ;;
        "Ollama Engine (Local)") OE="ON" ;;
    esac
done

# ── Step 3: Dependency enforcement ─────────────────────────────────────
DEP_MSG=""
if [ "$OL" = "ON" ]; then
    if [ "$OC" != "ON" ]; then
        OC="ON"
        DEP_MSG="$DEP_MSG  • OpenCode Local requires OpenCode CLI — enabled automatically.\n"
    fi
    if [ "$OE" != "ON" ]; then
        OE="ON"
        DEP_MSG="$DEP_MSG  • OpenCode Local requires Ollama Engine — enabled automatically.\n"
    fi
fi

# Validate at least one
count=0
for v in "$G_S" "$G_C" "$CL" "$CX" "$OC" "$OL" "$OE"; do
    [ "$v" = "ON" ] && ((count++)) || true
done

if [ "$count" -eq 0 ]; then
    osascript -e 'display dialog "At least one provider must be selected." buttons {"OK"} default button 1 with title "UAM Build Selector" with icon stop'
    exit 1
fi

# ── Step 4: Build config name ──────────────────────────────────────────
name_parts=()
[ "$G_S" = "ON" ] && name_parts+=("GeminiS")
[ "$G_C" = "ON" ] && name_parts+=("GeminiC")
[ "$CL" = "ON" ] && name_parts+=("Claude")
[ "$CX" = "ON" ] && name_parts+=("Codex")
[ "$OC" = "ON" ] && name_parts+=("OpenCodeCLI")
[ "$OL" = "ON" ] && name_parts+=("OpenCodeLocal")
[ "$OE" = "ON" ] && name_parts+=("OllamaEngine")

config_name=$(IFS="+"; echo "${name_parts[*]}")
out_dir="Builds/ProviderFlags/${config_name}"

# ── Step 5: Confirm ────────────────────────────────────────────────────
summary="Configuration: ${config_name}\n\n"
summary+="  Gemini (Structured): ${G_S}\n"
summary+="  Gemini (CLI):        ${G_C}\n"
summary+="  Claude Code:         ${CL}\n"
summary+="  Codex CLI:           ${CX}\n"
summary+="  OpenCode CLI:        ${OC}\n"
summary+="  OpenCode Local:      ${OL}\n"
summary+="  Ollama Engine:       ${OE}"

if [ -n "$DEP_MSG" ]; then
    summary="\n$DEP_MSG\n$summary"
fi

CONFIRM=$(osascript -e "
display dialog \"$summary\" buttons {\"Cancel\", \"Build\"} default button 2 with title \"UAM Build Selector\" with icon note
button returned of result
")

if [ "$CONFIRM" != "Build" ]; then
    echo "Build cancelled."
    exit 0
fi

# ── Step 6: Build ──────────────────────────────────────────────────────
echo ""
echo "Configuring: ${config_name}"
echo "Output:      ${out_dir}"
echo ""

cmake_args=(
    -S .
    -B "${out_dir}"
    -DUAM_FETCH_DEPS="${FETCH_DEPS}"
    -DUAM_FETCH_LLAMA_CPP="${FETCH_LLAMA}"
    -DUAM_BUILD_TESTS="${BUILD_TESTS}"
    -DUAM_ENABLE_RUNTIME_GEMINI_STRUCTURED="${G_S}"
    -DUAM_ENABLE_RUNTIME_GEMINI_CLI="${G_C}"
    -DUAM_ENABLE_RUNTIME_CODEX_CLI="${CX}"
    -DUAM_ENABLE_RUNTIME_CLAUDE_CLI="${CL}"
    -DUAM_ENABLE_RUNTIME_OPENCODE_CLI="${OC}"
    -DUAM_ENABLE_RUNTIME_OPENCODE_LOCAL="${OL}"
    -DUAM_ENABLE_RUNTIME_OLLAMA_ENGINE="${OE}"
)

if [ -n "${LLAMA_SRC}" ]; then
    cmake_args+=(-DUAM_LLAMA_CPP_SOURCE_DIR="${LLAMA_SRC}")
fi

cmake "${cmake_args[@]}"

echo ""
echo "Building..."
echo ""

cmake --build "${out_dir}" -j8

# ── Step 7: Done ───────────────────────────────────────────────────────
binary="${out_dir}/universal_agent_manager.app/Contents/MacOS/universal_agent_manager"

osascript -e "
display dialog \"Build complete!\" & return & return & \"Configuration: ${config_name}\" & return & \"Binary: ${binary}\" buttons {\"OK\"} default button 1 with title \"UAM Build Complete\" with icon note
"

echo ""
echo "Build complete: ${config_name}"
echo "Binary: ${binary}"
echo ""
