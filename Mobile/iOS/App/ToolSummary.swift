import Foundation

/// A bounded display preview. The original tool object remains the details/copy authority.
struct ToolSummary: Equatable, Sendable
{
    static let maximumTitleCharacters = 240
    private static let maximumInputBytes = 64 * 1024
    let displayTitle: String
    /// An optional command preview extracted from inline arguments, never fetched output.
    let command: String?

    init(_ tool: WireObject)
    {
        let subagent = tool.flag("isSubAgent")
        let kind = Self.nonempty(tool["kind"]) ?? ""
        let toolTitle = Self.nonempty(tool["title"])
        let execution = ["execute", "shell", "commandexecution"].contains(kind.lowercased())
        let extracted = execution && !subagent ? Self.inlineCommand(tool.text("content")) : nil
        command = extracted.map(Self.preview)
        let title = (subagent ? Self.nonempty(tool["subAgentTitle"]) : nil)
            ?? extracted ?? toolTitle
            ?? (subagent ? "Subagent activity" : Self.fallback(kind))
        displayTitle = Self.preview(title)
    }

    private static func nonempty(_ value: Any?) -> String?
    {
        guard let value = value as? String else { return nil }
        let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? nil : trimmed
    }

    private static func preview(_ value: String) -> String
    {
        let prefix = value.prefix(maximumTitleCharacters)
        return prefix.endIndex == value.endIndex ? value : String(prefix) + "…"
    }

    private static func fallback(_ kind: String) -> String
    {
        switch kind.lowercased()
        {
        case "execute", "shell", "commandexecution": "Run command"
        case "read": "Read file"
        case "write": "Write file"
        case "edit", "filechange": "Edit files"
        case "search", "websearch": "Search"
        case "", "other", "tool": "Tool activity"
        default: kind
        }
    }

    private static func inlineCommand(_ content: String) -> String?
    {
        // StateSerializer uses optional Approval, then Arguments JSON + Result.
        // Without the explicit prefix, JSON may be arguments OR a result: never
        // infer provenance from its keys. Never inspect Result/Child activity.
        var input = String(decoding: content.utf8.prefix(maximumInputBytes), as: UTF8.self)
            .trimmingCharacters(in: .whitespacesAndNewlines)
        if input.hasPrefix("Approval: ")
        {
            guard let end = input.range(of: "\n\n") else { return nil }
            input = String(input[end.upperBound...])
        }
        guard input.hasPrefix("Arguments:\n") else { return nil }
        input.removeFirst("Arguments:\n".count)
        for marker in ["\n\nResult:\n", "\n\nChild activity:\n"]
        {
            if let end = input.range(of: marker) { input = String(input[..<end.lowerBound]) }
        }
        return command(in: input, depth: 0)
    }

    private static func command(in value: Any, depth: Int) -> String?
    {
        guard depth < 5 else { return nil }
        if let object = value as? WireObject
        {
            if let command = nonempty(object["command"]) { return command }
            // Known provider input envelopes only; no recursive search of results.
            for key in ["rawInput", "arguments", "input", "parameters"]
            {
                if let nested = object[key], let command = command(in: nested, depth: depth + 1)
                { return command }
            }
        }
        else if let string = value as? String, string.utf8.count <= maximumInputBytes,
                let data = string.data(using: .utf8),
                let parsed = try? JSONSerialization.jsonObject(with: data, options: .fragmentsAllowed)
        { return command(in: parsed, depth: depth + 1) }
        return nil
    }
}
