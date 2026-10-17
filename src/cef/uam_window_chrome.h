#pragma once

namespace uam::cef
{
	/// macOS and Windows hide the system title bar; React draws the 44 px strip. Linux keeps its frame.
#if defined(__APPLE__) || defined(_WIN32)
	inline constexpr bool kCustomWindowChrome = true;
#else
	inline constexpr bool kCustomWindowChrome = false;
#endif

#if defined(__APPLE__)
	inline constexpr const char* kWindowChromePlatform = "macos";
#elif defined(_WIN32)
	inline constexpr const char* kWindowChromePlatform = "windows";
#else
	inline constexpr const char* kWindowChromePlatform = "linux";
#endif

	inline constexpr float kWindowStripHeight = 44.0f;
}
