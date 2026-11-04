import Foundation
import CoreFoundation

/// Navigation between saved alternatives at one user message, not an entire branch family.
struct MessageBranchGroup
{
    let parentID: String
    /// Absolute transcript index, including any history page's start offset.
    let messageIndex: Int
    let orderedMembers: [MobileChat]
    let currentIndex: Int

    var current: Int { currentIndex + 1 }
    var total: Int { orderedMembers.count }
    var previous: MobileChat? { currentIndex > 0 ? orderedMembers[currentIndex - 1] : nil }
    var next: MobileChat? { currentIndex + 1 < total ? orderedMembers[currentIndex + 1] : nil }

    /// Call only for a rendered user message, passing its absolute MobileMessage.index.
    static func build(chat: MobileChat, messageIndex: Int, chats: [MobileChat]) -> MessageBranchGroup?
    {
        guard messageIndex >= 0, messageIndex <= Int(Int32.max), eligible(chat) else { return nil }
        let parentID = !chat.parentID.isEmpty && forkIndex(chat) == messageIndex ? chat.parentID : chat.id
        var seen = Set<String>()
        let candidates = chats.enumerated().filter { _, candidate in
            guard seen.insert(candidate.id).inserted, eligible(candidate), candidate.familyID == chat.familyID else { return false }
            return candidate.id == parentID ||
                (candidate.parentID == parentID && forkIndex(candidate) == messageIndex)
        }.sorted {
            // Dates are cached by MobileChat. Desktop's stable createdAt sort retains
            // catalogue order for equal timestamps, including missing legacy dates.
            $0.element.createdAt == $1.element.createdAt
                ? $0.offset < $1.offset
                : $0.element.createdAt < $1.element.createdAt
        }.map(\.element)
        guard candidates.count > 1, let currentIndex = candidates.firstIndex(where: { $0.id == chat.id }) else { return nil }
        return MessageBranchGroup(parentID: parentID, messageIndex: messageIndex, orderedMembers: candidates, currentIndex: currentIndex)
    }

    private static func eligible(_ chat: MobileChat) -> Bool
    {
        !chat.id.isEmpty && chat.raw.text("temporaryParentChatId").isEmpty && chat.raw.text("subagentParentChatId").isEmpty
    }

    private static func forkIndex(_ chat: MobileChat) -> Int?
    {
        guard let number = chat.raw["branchFromMessageIndex"] as? NSNumber,
              CFGetTypeID(number) != CFBooleanGetTypeID() else { return nil }
        let value = number.doubleValue
        guard value.isFinite, value >= 0, value <= Double(Int32.max), value.rounded(.towardZero) == value else { return nil }
        return Int(value)
    }
}
