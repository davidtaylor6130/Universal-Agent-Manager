import os
import re

files = [
    "src/core/chat_import_utils.cpp",
    "src/app/application_core_helpers.cpp",
    "src/app/application_core_helpers.h",
    "src/common/config/frontend_actions.cpp",
    "src/common/provider/gemini/base/gemini_history_loader.cpp",
    "src/common/provider/markdown_template_catalog.cpp",
    "src/common/provider/opencode/opencode_history_service.cpp",
    "src/common/provider/opencode/local/opencode_local_bridge_service.cpp",
    "src/common/paths/app_paths.cpp",
    "src/common/rag/rag_index_service_common.inl",
    "src/common/vcs/vcs_workspace_service.cpp",
    "src/common/runtime/json_runtime.h"
]

def remove_trim(content, func_name="Trim"):
    pattern = r'(?:inline\s+)?std::string\s+' + func_name + r'\s*\(\s*(?:const\s+)?std::string\s*(?:&)?\s*\w+\s*\)\s*\{'
    match = re.search(pattern, content)
    if not match:
        # try without spaces or with std::string &
        pattern = r'(?:inline\s+)?std::string\s+' + func_name + r'\s*\(\s*(?:const\s+)?std::string\s*&\s*\w+\s*\)\s*\{'
        match = re.search(pattern, content)
        if not match:
            return content, False
    
    start_idx = match.start()
    idx = match.end() - 1
    brace_count = 1
    idx += 1
    while brace_count > 0 and idx < len(content):
        if content[idx] == '{':
            brace_count += 1
        elif content[idx] == '}':
            brace_count -= 1
        idx += 1
    
    return content[:start_idx] + content[idx:], True

for fpath in files:
    if not os.path.exists(fpath):
        continue
    with open(fpath, 'r') as f:
        content = f.read()
    
    fname = "JsonTrim" if "json_runtime.h" in fpath else "Trim"
    new_content, modified = remove_trim(content, fname)
    if "json_runtime.h" in fpath:
        new_content = new_content.replace('JsonTrim', 'Trim')
        
    if modified:
        if "string_utils.h" not in new_content and not fpath.endswith(".h") and not fpath.endswith(".inl"):
            includes = list(re.finditer(r'#include\s+.*?\n', new_content))
            if includes:
                last_inc = includes[-1]
                new_content = new_content[:last_inc.end()] + '#include "common/utils/string_utils.h"\n' + new_content[last_inc.end():]
        with open(fpath, 'w') as f:
            f.write(new_content)
        print(f"Modified {fpath}")
