import Foundation

struct TranscriptSegment: Identifiable, Equatable
{
    enum Kind: String { case text, thought, tools, compaction, plan }
    let id: Int
    let kind: Kind
    var text = ""
    var toolIDs: [String] = []
}

enum TranscriptSegments
{
    static func build(_ message: MobileMessage) -> [TranscriptSegment]
    {
        let blocks = message.raw.objects("blocks")
        var result: [TranscriptSegment] = []
        var seenTools = Set<String>()
        var hasText = false
        func append(_ kind: TranscriptSegment.Kind, text: String = "", toolID: String? = nil)
        {
            if kind == .text, result.last?.kind == .text { result[result.count - 1].text += text; return }
            if kind == .tools, result.last?.kind == .tools, let toolID { result[result.count - 1].toolIDs.append(toolID); return }
            result.append(TranscriptSegment(id: result.count, kind: kind, text: text, toolIDs: toolID.map { [$0] } ?? []))
        }
        for block in blocks
        {
            switch block.text("type")
            {
            case "assistant_text":
                hasText = true
                append(.text, text: block.text("text"))
            case "thought":
                if !block.text("text").isEmpty { append(.thought, text: block.text("text")) }
            case "tool_call":
                let id = block.text("toolCallId")
                if !id.isEmpty, seenTools.insert(id).inserted { append(.tools, toolID: id) }
            case "context_compaction", "context_compaction_interrupted":
                append(.compaction, text: block.text("type") == "context_compaction_interrupted" ? "Context compaction interrupted" : "Context compacted")
            case "plan": append(.plan, text: message.raw.text("planSummary"))
            default: break
            }
        }
        if !hasText, !message.content.isEmpty { append(.text, text: message.content) }
        if !blocks.contains(where: { $0.text("type") == "thought" }), !message.raw.text("thoughts").isEmpty
        { append(.thought, text: message.raw.text("thoughts")) }
        for tool in message.tools where seenTools.insert(tool.text("id")).inserted { append(.tools, toolID: tool.text("id")) }
        return result
    }
}
