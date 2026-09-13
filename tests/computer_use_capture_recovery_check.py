#!/usr/bin/env python3
"""Compile and exercise the production capture-failure method in a tiny stub harness."""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/computer_use/computer_use_mcp_server.cpp"


def extract_method(source: str, marker: str, next_marker: str) -> str:
    start = source.index(marker)
    end = source.index(next_marker, start)
    return source[start:end].rstrip()


HARNESS = (r'''#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

struct Capture { std::string error; };
struct Target {
    std::string kind;
    std::uint64_t id = 0;
    std::uint64_t process_id = 0;
    std::string title;
};

namespace uam::strings {
static std::string Trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\n\r");
    if (first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\n\r");
    return value.substr(first, last - first + 1);
}
static std::string ToLowerAscii(std::string value)
{
    for (char& character : value)
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    return value;
}
static std::string SafeLine(std::string value, std::size_t, bool)
{
    return value;
}
}

class Harness {
public:
    std::string m_targetKind = "window";
    std::uint64_t m_targetId = 42;
    std::uint64_t m_targetProcessId = 7;
    Capture m_lastCapture{"stale screenshot"};
    std::string m_lastElementText = "stale elements";
    std::uint64_t m_frameSerial = 10;
    std::vector<Target> listed_targets;
    std::vector<Target> visible_targets;
    bool use_visible_targets = false;
    std::string enumeration_error;
    std::optional<std::string> policy_error;
    bool revoked = false;
    std::vector<std::string> records;

    std::vector<Target> ListTargets(std::string* error) const
    {
        if (error != nullptr)
            *error = enumeration_error;
        return listed_targets;
    }

    std::vector<Target> VisibleTargets(const std::vector<Target>& targets) const
    {
        return use_visible_targets ? visible_targets : targets;
    }

    std::string FormatTargetChoices(const std::vector<Target>&) const
    {
        return {};
    }

    std::optional<std::string> ValidateTargetPolicy(const Target&) const
    {
        return policy_error;
    }

    std::string RevokeGrant(std::string reason)
    {
        revoked = true;
        return reason + " revoked";
    }

    void Record(std::string, std::string, std::string detail)
    {
        records.push_back(std::move(detail));
    }

''' + r'''RESOLVE_METHOD
''' + r'''METHOD''' + r'''    bool Cleared() const
    {
        return m_lastCapture.error.empty() && m_lastElementText.empty();
    }
};

static Target CurrentTarget(std::uint64_t process_id = 7, std::string title = "")
{
    return {"window", 42, process_id, std::move(title)};
}

static bool Check(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << "\n";
    return condition;
}

int main()
{
    bool ok = true;
    {
        Harness h;
        h.listed_targets = {CurrentTarget(7, "Firefox — UAM Canvas Input Test")};
        std::string error;
        const auto target = h.ResolveRequestedTarget("42", &error);
        ok &= Check(target && target->id == 42 && error.empty(), "exact ID must resolve");
    }
    {
        Harness h;
        h.listed_targets = {CurrentTarget(7, "Firefox — UAM Canvas Input Test")};
        std::string error;
        const auto target = h.ResolveRequestedTarget("Firefox UAM Canvas Input Test", &error);
        ok &= Check(target && target->id == 42 && error.empty(), "title without separator must resolve");
    }
    {
        Harness h;
        h.listed_targets = {
            CurrentTarget(7, "Firefox — First Window"),
            {"window", 43, 7, "Firefox — Second Window"},
        };
        std::string error;
        const auto target = h.ResolveRequestedTarget("firefox", &error);
        ok &= Check(!target && error.find("ambiguous") != std::string::npos, "ambiguous same-app query must reject");
    }
    {
        Harness h;
        h.listed_targets = {CurrentTarget(7, "Firefox — UAM Canvas Input Test")};
        std::string error;
        const auto target = h.ResolveRequestedTarget("unknown application", &error);
        ok &= Check(!target && error.find("No available computer-use target matches") != std::string::npos, "unknown query must reject");
    }
    {
        Harness h;
        h.listed_targets = {CurrentTarget()};
        const std::string result = h.HandleCaptureFailure(Capture{"temporary"});
        ok &= Check(!h.revoked && h.Cleared() && h.m_frameSerial == 11 && result.find("Observe again") != std::string::npos, "existing target must retain grant");
    }
    {
        Harness h;
        const std::string result = h.HandleCaptureFailure(Capture{"gone"});
        ok &= Check(h.revoked && h.Cleared() && result.find("revoked") != std::string::npos, "missing target must revoke");
    }
    {
        Harness h;
        h.listed_targets = {CurrentTarget(8)};
        h.m_targetProcessId = 7;
        ok &= Check(h.HandleCaptureFailure(Capture{"pid changed"}).find("revoked") != std::string::npos && h.revoked, "PID mismatch must revoke");
    }
    {
        Harness h;
        h.listed_targets = {CurrentTarget()};
        h.policy_error = "blocked by policy";
        ok &= Check(h.HandleCaptureFailure(Capture{"policy"}).find("revoked") != std::string::npos && h.revoked, "policy block must revoke");
    }
    {
        Harness h;
        h.enumeration_error = "temporary enumeration failure";
        const std::string result = h.HandleCaptureFailure(Capture{"enumeration"});
        ok &= Check(!h.revoked && h.Cleared() && result.find("Observe again") != std::string::npos, "enumeration failure must retain grant");
    }
    return ok ? 0 : 1;
}
''')


def main() -> int:
    compiler = shutil.which("clang++") or shutil.which("c++")
    if compiler is None:
        print("no C++ compiler found", file=sys.stderr)
        return 2
    source = SOURCE.read_text()
    resolve_method = extract_method(
        source,
        "\t\t\tstd::optional<Target> ResolveRequestedTarget(std::string query, std::string* error_out) const",
        "\t\t\tstd::optional<std::string> ValidateTargetPolicy(const Target& target, const AppSettings& settings) const",
    )
    capture_method = extract_method(
        source,
        "\t\t\tstd::string HandleCaptureFailure(const Capture& failed_capture)",
        "\t\t\tnlohmann::json CallTool(const std::string& name, const nlohmann::json& arguments)",
    )
    harness = HARNESS.replace("RESOLVE_METHOD", resolve_method).replace("METHOD", capture_method)
    with tempfile.TemporaryDirectory(prefix="uam-capture-recovery-") as directory:
        directory_path = pathlib.Path(directory)
        source_path = directory_path / "check.cpp"
        binary_path = directory_path / "check"
        source_path.write_text(harness)
        compile_result = subprocess.run(
            [compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(source_path), "-o", str(binary_path)],
            capture_output=True,
            text=True,
        )
        if compile_result.returncode:
            print(compile_result.stderr, file=sys.stderr, end="")
            return compile_result.returncode
        run_result = subprocess.run([str(binary_path)], capture_output=True, text=True)
        if run_result.stdout:
            print(run_result.stdout, end="")
        if run_result.stderr:
            print(run_result.stderr, file=sys.stderr, end="")
        return run_result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
