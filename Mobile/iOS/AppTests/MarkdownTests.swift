import XCTest
@testable import UAM

final class MarkdownTests: XCTestCase
{
    func testHeadingTextAndFollowingParagraphSurvive()
    {
        XCTAssertEqual(MarkdownBlocks.parse("## Keep this heading\nFollowing text"), [
            .paragraph(body: "Keep this heading", kind: .heading(level: 2)),
            .paragraph(body: "Following text", kind: .normal)])
    }

    func testLanguageFenceClosesAndFollowingTextRemainsProse()
    {
        XCTAssertEqual(MarkdownBlocks.parse("Before\n\n```swift\nlet x = 1\n```\n\nAfter"), [
            .paragraph(body: "Before", kind: .normal), .code(language: "swift", source: "let x = 1"),
            .paragraph(body: "After", kind: .normal)])
    }

    func testStreamingFenceAndIndentedCodeArePreserved()
    {
        XCTAssertEqual(MarkdownBlocks.parse("~~~js\n  run()"), [.code(language: "js", source: "  run()")])
    }

    func testListsAndQuotesPreserveContent()
    {
        XCTAssertEqual(MarkdownBlocks.parse("- first\n* second\n\n> quoted\n> text"), [
            .paragraph(body: "• first", kind: .normal), .paragraph(body: "• second", kind: .normal),
            .paragraph(body: "quoted\ntext", kind: .quote)])
    }

    func testTableHeadersAlignmentsAndInlineSourceArePreserved()
    {
        XCTAssertEqual(MarkdownBlocks.parse("| **Name** | Count | State |\n| :--- | ---: | :---: |\n| [task](https://example.com) | 12 | `ready` |"), [
            .table(headers: ["**Name**", "Count", "State"], alignments: [.leading, .trailing, .center],
                   rows: [["[task](https://example.com)", "12", "`ready`"]])])
    }

    func testDelimiterAcceptsShortHyphenRuns()
    {
        XCTAssertEqual(MarkdownBlocks.parse("A | B\n- | :-:\nleft | middle"), [
            .table(headers: ["A", "B"], alignments: [.leading, .center], rows: [["left", "middle"]])])
    }

    func testTableCellsDoNotSplitEscapedOrCodeSpanPipes()
    {
        XCTAssertEqual(MarkdownBlocks.parse(#"A | B"# + "\n--- | ---\n" + #"left\|right | `x|y`"# + "\n" + #"``x`|y`` | done"#), [
            .table(headers: ["A", "B"], alignments: [.leading, .leading],
                   rows: [[#"left\|right"#, "`x|y`"], ["``x`|y``", "done"]])])
    }

    func testInvalidTableDelimiterRemainsParagraph()
    {
        for delimiter in [":: | ---", "--- | invalid", "--- | --- | ---"]
        {
            XCTAssertEqual(MarkdownBlocks.parse("A | B\n" + delimiter), [
                .paragraph(body: "A | B\n" + delimiter, kind: .normal)])
        }
    }

    func testOneColumnTableAndShortRowsKeepFollowingContent()
    {
        XCTAssertEqual(MarkdownBlocks.parse("| Header |\n| --- |\n| Cell |\n\nAfter"), [
            .table(headers: ["Header"], alignments: [.leading], rows: [["Cell"]]),
            .paragraph(body: "After", kind: .normal)])
        XCTAssertEqual(MarkdownBlocks.parse("A | B\n--- | ---\n| only |"), [
            .table(headers: ["A", "B"], alignments: [.leading, .leading], rows: [["only", ""]])])
    }

    func testNestedUnorderedAndOrderedListsRetainIndentAndNumbers()
    {
        XCTAssertEqual(MarkdownBlocks.parse("- parent\n  - nested\n    2. deeper\n  3) sibling\n+ next"), [
            .paragraph(body: "• parent", kind: .normal),
            .paragraph(body: "• nested", kind: .list(indent: 2)),
            .paragraph(body: "2. deeper", kind: .list(indent: 4)),
            .paragraph(body: "3. sibling", kind: .list(indent: 2)),
            .paragraph(body: "• next", kind: .normal)])
    }

    func testSeparatorsAndUnterminatedFenceAfterTableRemainDistinct()
    {
        XCTAssertEqual(MarkdownBlocks.parse("A | B\n--- | ---\n1 | 2\n\n- - -\n\n```swift\n  let text = \"a|b\""), [
            .table(headers: ["A", "B"], alignments: [.leading, .leading], rows: [["1", "2"]]),
            .separator, .code(language: "swift", source: "  let text = \"a|b\"")])
        XCTAssertEqual(MarkdownBlocks.parse("***\n___"), [.separator, .separator])
    }

}
