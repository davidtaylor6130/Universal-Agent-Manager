import SwiftUI

/// Native Markdown rendering for one message: selectable inline Markdown paragraphs
/// and fenced code blocks with horizontal scrolling. Parsing is local to this view,
/// so streaming elsewhere in the transcript never triggers a whole-transcript reparse.
struct MarkdownContent: View
{
    let text: String

    var body: some View
    {
        VStack(alignment: .leading, spacing: 10)
        {
            ForEach(Array(blocks.enumerated()), id: \.offset) { _, block in content(for: block) }
        }
    }

    private var blocks: [MarkdownBlock] { MarkdownBlocks.parse(text) }

    @ViewBuilder
    private func content(for block: MarkdownBlock) -> some View
    {
        switch block
        {
        case .paragraph(let bodyText, let kind):
            MarkdownParagraph(bodyText: bodyText, kind: kind)
        case .code(let language, let source):
            MarkdownCodeBlock(language: language, source: source)
        case .table(let headers, let alignments, let rows):
            MarkdownTable(headers: headers, alignments: alignments, rows: rows)
        case .separator:
            Divider().padding(.vertical, 4)
        }
    }
}


enum MarkdownBlock: Equatable
{
    case paragraph(body: String, kind: ParagraphKind)
    case code(language: String, source: String)
    case table(headers: [String], alignments: [MarkdownColumnAlignment], rows: [[String]])
    case separator
}

enum MarkdownColumnAlignment: Equatable
{
    case leading, center, trailing
}

enum MarkdownBlocks
{
    static func parse(_ text: String) -> [MarkdownBlock]
    {
        var result: [MarkdownBlock] = []
        var paragraph: [String] = []
        var code: [String] = []
        var fenceCharacter: Character?
        var fenceLength = 0
        var language = ""
        func flush()
        {
            guard !paragraph.isEmpty else { return }
            let quote = paragraph.allSatisfy { $0.trimmingCharacters(in: .whitespaces).hasPrefix(">") }
            let lines = quote ? paragraph.map { String($0.trimmingCharacters(in: .whitespaces).dropFirst()).trimmingCharacters(in: .whitespaces) } : paragraph
            result.append(.paragraph(body: lines.joined(separator: "\n"), kind: quote ? .quote : .normal))
            paragraph = []
        }
        let lines = text.components(separatedBy: "\n")
        var index = 0
        while index < lines.count
        {
            let line = lines[index]
            index += 1
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if let character = fenceCharacter
            {
                let run = trimmed.prefix { $0 == character }
                if run.count >= fenceLength && trimmed.dropFirst(run.count).trimmingCharacters(in: .whitespaces).isEmpty
                {
                    result.append(.code(language: language, source: code.joined(separator: "\n")))
                    code = []; fenceCharacter = nil
                }
                else { code.append(line) }
                continue
            }
            if let character = trimmed.first, character == "`" || character == "~"
            {
                let run = trimmed.prefix { $0 == character }
                if run.count >= 3
                {
                    flush(); fenceCharacter = character; fenceLength = run.count
                    language = String(trimmed.dropFirst(run.count)).trimmingCharacters(in: .whitespaces)
                    continue
                }
            }
            if index < lines.count, let headers = tableCells(line), let delimiters = tableCells(lines[index]),
               headers.count == delimiters.count,
               delimiters.allSatisfy({ $0.range(of: "^:?-+:?$", options: .regularExpression) != nil })
            {
                flush(); index += 1
                let alignments: [MarkdownColumnAlignment] = delimiters.map {
                    $0.hasSuffix(":") ? ($0.hasPrefix(":") ? .center : .trailing) : .leading
                }
                var rows: [[String]] = []
                while index < lines.count, let cells = tableCells(lines[index])
                {
                    rows.append(Array(cells.prefix(headers.count)) + Array(repeating: "", count: max(0, headers.count - cells.count)))
                    index += 1
                }
                result.append(.table(headers: headers, alignments: alignments, rows: rows))
                continue
            }
            let rule = trimmed.filter { !$0.isWhitespace }
            if rule.count >= 3, let character = rule.first, "-*_".contains(character), rule.allSatisfy({ $0 == character })
            { flush(); result.append(.separator); continue }
            let hashes = trimmed.prefix { $0 == "#" }
            if (1...6).contains(hashes.count), trimmed.dropFirst(hashes.count).hasPrefix(" ")
            {
                flush()
                result.append(.paragraph(body: String(trimmed.dropFirst(hashes.count)).trimmingCharacters(in: .whitespaces), kind: .heading(level: hashes.count)))
            }
            else if trimmed.isEmpty { flush() }
            else if let item = listItem(line)
            {
                flush()
                result.append(.paragraph(body: item.body, kind: item.indent == 0 && item.unordered ? .normal : .list(indent: item.indent)))
            }
            else { paragraph.append(line) }
        }
        flush()
        if fenceCharacter != nil { result.append(.code(language: language, source: code.joined(separator: "\n"))) }
        return result
    }

    /// Split outside code spans while keeping escapes for the inline Markdown renderer.
    static func tableCells(_ line: String) -> [String]?
    {
        let characters = Array(line.trimmingCharacters(in: .whitespaces))
        var cells: [String] = [], cell = "", index = 0, ticks = 0, hasPipe = false
        func closingTicks(after start: Int, count: Int) -> Bool
        {
            var cursor = start
            while cursor < characters.count
            {
                if characters[cursor] != "`" { cursor += 1; continue }
                var end = cursor
                while end < characters.count && characters[end] == "`" { end += 1 }
                if end - cursor == count { return true }
                cursor = end
            }
            return false
        }
        while index < characters.count
        {
            let character = characters[index]
            if ticks == 0, character == "\\", index + 1 < characters.count
            { cell.append(character); cell.append(characters[index + 1]); index += 2; continue }
            if character == "`"
            {
                var end = index
                while end < characters.count && characters[end] == "`" { end += 1 }
                let count = end - index
                if ticks == count { ticks = 0 }
                else if ticks == 0, closingTicks(after: end, count: count) { ticks = count }
                cell += String(repeating: "`", count: count); index = end; continue
            }
            if character == "|", ticks == 0 { cells.append(cell); cell = ""; hasPipe = true }
            else { cell.append(character) }
            index += 1
        }
        guard hasPipe else { return nil }
        cells.append(cell)
        if cells.first?.isEmpty == true { cells.removeFirst() }
        if cells.last?.isEmpty == true { cells.removeLast() }
        return cells.isEmpty ? nil : cells.map { $0.trimmingCharacters(in: .whitespaces) }
    }

    private static func listItem(_ line: String) -> (body: String, indent: Int, unordered: Bool)?
    {
        let indent = line.prefix { $0 == " " || $0 == "\t" }.reduce(0) { $0 + ($1 == "\t" ? 4 : 1) }
        let trimmed = line.trimmingCharacters(in: .whitespaces)
        if trimmed.hasPrefix("- ") || trimmed.hasPrefix("* ") || trimmed.hasPrefix("+ ")
        { return ("• " + trimmed.dropFirst(2), indent, true) }
        let digits = trimmed.prefix { $0.isASCII && $0.isNumber }
        let suffix = trimmed.dropFirst(digits.count)
        if (1...9).contains(digits.count), suffix.hasPrefix(". ") || suffix.hasPrefix(") ")
        { return (String(digits) + ". " + suffix.dropFirst(2), indent, false) }
        return nil
    }

}

// MARK: Paragraph

enum ParagraphKind: Equatable
{
    case normal
    case heading(level: Int)
    case quote
    case list(indent: Int)
}

/// A single rendered block of text. Inline Markdown is parsed with
/// inlineOnlyPreservingWhitespace so whitespace and readability are preserved;
/// links are guarded to http/https so tappable custom schemes are impossible.
private struct MarkdownParagraph: View
{
    let bodyText: String
    let kind: ParagraphKind

    private static let inlineOptions = AttributedString.MarkdownParsingOptions(
        interpretedSyntax: .inlineOnlyPreservingWhitespace)

    private var attributed: AttributedString
    {
        let parsed = (try? AttributedString(markdown: bodyText, options: Self.inlineOptions))
            ?? AttributedString(bodyText)
        return Self.stripUnsafeLinks(from: parsed)
    }

    /// Only http/https links survive; everything else renders as plain text.
    static func stripUnsafeLinks(from source: AttributedString) -> AttributedString
    {
        var attributed = source
        for run in attributed.runs
        {
            guard let url = run.link?.absoluteString.lowercased() else { continue }
            if !url.hasPrefix("https://") && !url.hasPrefix("http://")
            {
                attributed[run.range].link = nil
            }
        }
        return attributed
    }

    var body: some View
    {
        Text(attributed)
            .font(font)
            .foregroundStyle(isQuote ? Color.secondary : Color.primary)
            .italic(isQuote)
            .lineSpacing(4)
            .textSelection(.enabled)
            .padding(.leading, listPadding)
            .environment(\.openURL, OpenURLAction { url in
                let scheme = url.scheme?.lowercased() ?? ""
                if scheme == "http" || scheme == "https" { return .systemAction }
                return .discarded
            })
    }

    private var listPadding: CGFloat
    {
        if case .list(let indent) = kind { return CGFloat(min(indent, 24)) * 8 }
        return 0
    }

    private var isQuote: Bool
    {
        if case .quote = kind { return true }
        return false
    }

    private var font: Font
    {
        if case .heading(let level) = kind
        {
            switch level
            {
            case 1: return .system(.title2, weight: .semibold)
            case 2: return .system(.title3, weight: .semibold)
            default: return .headline
            }
        }
        return .body
    }
}

// MARK: Horizontal overflow

/// The system indicator and a restrained footer reveal overflow only after the real scroll
/// geometry confirms it. Keeping one ScrollView preserves selection and scroll identity.
private struct MarkdownHorizontalScroll<Content: View>: View
{
    let identifier: String
    let label: String
    @ViewBuilder let content: Content
    @State private var position = HorizontalPosition()

    private struct HorizontalPosition: Equatable
    {
        var overflows = false
        var percent = 0
    }

    var body: some View
    {
        VStack(alignment: .leading, spacing: 0)
        {
            ScrollView(.horizontal) { content }
                .scrollIndicators(.visible, axes: .horizontal)
                .scrollIndicatorsFlash(trigger: position.overflows)
                .onScrollGeometryChange(for: HorizontalPosition.self) { geometry in
                    let extent = geometry.contentSize.width - geometry.containerSize.width
                    guard extent > 1 else { return HorizontalPosition() }
                    let offset = geometry.contentOffset.x + geometry.contentInsets.leading
                    let percent = Int((min(1, max(0, offset / extent)) * 100).rounded())
                    return HorizontalPosition(overflows: true, percent: percent)
                } action: { _, current in position = current }
                .accessibilityIdentifier(identifier)
                .accessibilityLabel(label)
                .accessibilityHint(position.overflows ? "Scroll horizontally to read all content." : "")
                .accessibilityValue(position.overflows ? "Horizontal position \(position.percent) percent" : "")
            if position.overflows
            {
                Label("Scroll horizontally", systemImage: "arrow.left.and.right")
                    .font(.caption2).foregroundStyle(.secondary)
                    .padding(.horizontal, 12).padding(.top, 4).padding(.bottom, 6)
                    .accessibilityIdentifier(identifier + "-overflowHint")
            }
        }
    }
}

// MARK: Tables

private struct MarkdownTable: View
{
    let headers: [String]
    let alignments: [MarkdownColumnAlignment]
    let rows: [[String]]

    var body: some View
    {
        MarkdownHorizontalScroll(identifier: "markdownTableScroll", label: "Table")
        {
            Grid(horizontalSpacing: 0, verticalSpacing: 0)
            {
                tableRow(headers, header: true)
                ForEach(Array(rows.enumerated()), id: \.offset) { _, cells in tableRow(cells, header: false) }
            }.fixedSize(horizontal: true, vertical: false)
        }.background(UAMTheme.surface).clipShape(.rect(cornerRadius: 10))
            .overlay { RoundedRectangle(cornerRadius: 10).stroke(.quaternary) }
    }

    private func tableRow(_ cells: [String], header: Bool) -> some View
    {
        GridRow(alignment: .top)
        {
            ForEach(headers.indices, id: \.self) { column in
                let alignment = alignments[column]
                MarkdownParagraph(bodyText: cells[column], kind: .normal)
                    .fontWeight(header ? .semibold : nil)
                    .multilineTextAlignment(alignment == .center ? .center : alignment == .trailing ? .trailing : .leading)
                    .frame(minWidth: 100, maxWidth: 240, alignment: alignment == .center ? .center : alignment == .trailing ? .trailing : .leading)
                    .padding(10)
                    .background(header ? UAMTheme.raised : UAMTheme.surface)
                    .overlay(alignment: .bottom) { Divider() }
                    .accessibilityAddTraits(header ? .isHeader : [])
            }
        }
    }
}

// MARK: Code block

/// Monospace fenced block with a language label, copy button and horizontal
/// scrolling; long lines scroll instead of wrapping or resizing the layout.
private struct MarkdownCodeBlock: View
{
    let language: String
    let source: String
    @State private var copied = false

    var body: some View
    {
        VStack(alignment: .leading, spacing: 0)
        {
            HStack
            {
                Text(language.isEmpty ? "code" : language)
                    .font(.caption.monospaced())
                    .foregroundStyle(Color.secondary)
                    .textSelection(.enabled)
                Spacer()
                Button
                {
                    UIPasteboard.general.string = source
                    copied = true
                    Task(priority: .userInitiated)
                    { @MainActor in
                        try? await Task.sleep(nanoseconds: 1_600_000_000)
                        copied = false
                    }
                } label:
                {
                    Label(copied ? "Copied" : "Copy",
                          systemImage: copied ? "checkmark" : "doc.on.doc")
                        .font(.caption.weight(copied ? .semibold : .medium))
                        .foregroundStyle(copied ? Color.green : UAMTheme.accent)
                }
                .buttonStyle(.plain)
                .accessibilityLabel(copied ? "Code copied" : "Copy code")
            }
            .padding(.horizontal, 12)
            .padding(.top, 7)
            .padding(.bottom, 5)

            Divider()
            MarkdownHorizontalScroll(identifier: "markdownCodeScroll", label: "Code block")
            {
                Text(source)
                    .accessibilityIdentifier("markdownCodeContent")
                    .font(.system(size: 14, design: .monospaced))
                    .lineSpacing(3)
                    .textSelection(.enabled)
                    .foregroundStyle(Color.primary)
                    .padding(12)
                    .fixedSize(horizontal: true, vertical: false)
            }
        }
        .background(UAMTheme.surface)
        .clipShape(RoundedRectangle(cornerRadius: 10))
    }
}
