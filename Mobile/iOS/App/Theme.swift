import SwiftUI

/// Native equivalents of the desktop Focus theme; semantic colours adapt to light mode.
enum UAMTheme
{
    static let accent = Color(red: 207 / 255, green: 117 / 255, blue: 56 / 255)
    static let background = adaptive(dark: 0x0b0c0f, light: 0xf7f8fa)
    static let surface = adaptive(dark: 0x171a21, light: 0xffffff)
    static let raised = adaptive(dark: 0x20242d, light: 0xebeef3)
    static let muted = Color.secondary
    static let gold = Color(red: 0.82, green: 0.66, blue: 0.37)

    private static func adaptive(dark: UInt32, light: UInt32) -> Color
    {
        Color(uiColor: UIColor { traits in
            let hex = traits.userInterfaceStyle == .dark ? dark : light
            return UIColor(red: CGFloat((hex >> 16) & 255) / 255,
                           green: CGFloat((hex >> 8) & 255) / 255,
                           blue: CGFloat(hex & 255) / 255, alpha: 1)
        })
    }
}

struct RoundButton: View
{
    let title: String
    let symbol: String
    var tint: Color = .primary
    var action: () -> Void

    var body: some View
    {
        Button(action: action)
        {
            Image(systemName: symbol).font(.system(size: 20, weight: .regular))
                .foregroundStyle(tint).frame(width: 46, height: 46)
        }
        .buttonStyle(.plain)
        .glassEffect(.regular.interactive(), in: .circle)
        .accessibilityLabel(title)
    }
}

struct EmptyContent: View
{
    let title: String
    let message: String
    var symbol: String = "bubble.left.and.bubble.right"

    var body: some View
    {
        VStack(spacing: 12)
        {
            Image(systemName: symbol).font(.system(size: 32)).foregroundStyle(.secondary)
            Text(title).font(.title3.weight(.semibold))
            Text(message).font(.subheadline).foregroundStyle(.secondary)
                .multilineTextAlignment(.center).frame(maxWidth: 310)
        }
        .padding(28).frame(maxWidth: .infinity)
    }
}

struct ProviderMark: View
{
    let id: String
    var size: CGFloat = 19
    var body: some View
    {
        Group
        {
            if let brand
            {
                Image("Provider-" + brand).resizable()
                    // OpenCode is a two-tone mark; its appearance-aware vector asset must retain both fills.
                    .renderingMode(["codex", "copilot"].contains(brand) ? .template : .original)
                    .scaledToFit().frame(width: size, height: size)
            }
            else { Image(systemName: "circle.hexagongrid").font(.system(size: size, weight: .medium)) }
        }.foregroundStyle(.secondary).accessibilityLabel(providerName(id))
    }
    private var brand: String?
    {
        ["claude", "gemini", "copilot", "antigravity", "opencode", "codex"].first { id.contains($0) }
    }
}

func providerName(_ id: String) -> String
{
    if id.contains("claude") { return "Claude" }
    if id.contains("gemini") { return "Gemini" }
    if id.contains("copilot") { return "GitHub Copilot" }
    if id.contains("antigravity") { return "Antigravity" }
    if id.contains("opencode") { return "OpenCode" }
    if id.contains("codex") { return "Codex" }
    return id.isEmpty ? "Provider" : id
}
