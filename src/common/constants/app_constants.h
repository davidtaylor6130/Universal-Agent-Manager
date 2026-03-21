#pragma once

namespace uam::constants {

/// <summary>
/// Default folder identifier used for chats without an explicit folder assignment.
/// </summary>
inline constexpr const char* kDefaultFolderId = "folder-default";

/// <summary>
/// Default folder title shown in the sidebar.
/// </summary>
inline constexpr const char* kDefaultFolderTitle = "General";

/// <summary>
/// Application display name rendered in the UI and window title.
/// </summary>
inline constexpr const char* kAppDisplayName = "Universal Agent Manager";

/// <summary>
/// User-facing application version string.
/// </summary>
inline constexpr const char* kAppVersion = "1.0.0";

/// <summary>
/// Copyright footer displayed in About dialogs.
/// </summary>
inline constexpr const char* kAppCopyright = "(c) 2026 David Taylor (davidtaylor6130). All rights reserved.";

/// <summary>
/// Supported Gemini CLI version expected by this build.
/// </summary>
inline constexpr const char* kSupportedGeminiVersion = "0.30.0";

}  // namespace uam::constants
