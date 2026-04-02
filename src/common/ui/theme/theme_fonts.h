#pragma once
#include "common/platform/platform_services.h"

/// <summary>
/// Font range and font-loading configuration helpers for Dear ImGui.
/// </summary>
static const ImWchar* BuildTerminalGlyphRanges(ImGuiIO& io)
{
	static ImVector<ImWchar> ranges;

	if (!ranges.empty())
	{
		return ranges.Data;
	}

	ImFontGlyphRangesBuilder builder;
	builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
	builder.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
	builder.AddRanges(io.Fonts->GetGlyphRangesGreek());
	builder.AddRanges(io.Fonts->GetGlyphRangesVietnamese());
	builder.AddRanges(io.Fonts->GetGlyphRangesThai());
	builder.AddRanges(io.Fonts->GetGlyphRangesJapanese());
	builder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
	builder.AddRanges(io.Fonts->GetGlyphRangesKorean());
	static const ImWchar kTerminalExtras[] = {
	    0x2190, 0x21FF, // arrows
	    0x2300, 0x23FF, // misc technical
	    0x2500, 0x259F, // box drawing + block elements
	    0x25A0, 0x25FF, // geometric shapes
	    0x2600, 0x26FF, // misc symbols
	    0x2700, 0x27BF, // dingbats
	    0x2B00, 0x2BFF, // additional arrows/symbols
	    0xFFFD, 0xFFFD, // replacement char
	    0xE0B0, 0xE0D4, // powerline symbols (private use)
	    0,
	};

	builder.AddRanges(kTerminalExtras);
	builder.BuildRanges(&ranges);
	return ranges.Data;
}

static bool MergeFontIntoLast(ImGuiIO& io, const float size, std::initializer_list<const char*> paths, const ImWchar* glyph_ranges)
{
	ImFontConfig merge_cfg{};
	merge_cfg.MergeMode = true;
	merge_cfg.PixelSnapH = false;
	merge_cfg.OversampleH = 1;
	merge_cfg.OversampleV = 1;

	for (const char* path : paths)
	{
		if (path != nullptr && fs::exists(path))
		{
			if (io.Fonts->AddFontFromFileTTF(path, size, &merge_cfg, glyph_ranges) != nullptr)
			{
				return true;
			}
		}
	}

	return false;
}

static ImFont* TryLoadFont(ImGuiIO& io, const float size, std::initializer_list<const char*> paths, const ImWchar* glyph_ranges = nullptr)
{
	for (const char* path : paths)
	{
		if (path != nullptr && fs::exists(path))
		{
			if (ImFont* font = io.Fonts->AddFontFromFileTTF(path, size, nullptr, glyph_ranges))
			{
				return font;
			}
		}
	}

	return nullptr;
}

static void ConfigureFonts(ImGuiIO& io, const float dpi_scale = 1.0f)
{
	const float scale = std::clamp(dpi_scale, 1.0f, 2.25f);
	const float ui_font_size = 14.0f * scale;
	const float title_font_size = 18.5f * scale;
	const float mono_font_size = 13.5f * scale;
	const ImWchar* ui_ranges = io.Fonts->GetGlyphRangesDefault();
	const ImWchar* terminal_ranges = BuildTerminalGlyphRanges(io);
	g_font_ui = TryLoadFont(io, ui_font_size, {"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/segoeuii.ttf", "C:/Windows/Fonts/arial.ttf", "/System/Library/Fonts/SFNS.ttf", "/Library/Fonts/Inter-Regular.ttf", "/System/Library/Fonts/Supplemental/Helvetica.ttc", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"}, ui_ranges);
	g_font_title = TryLoadFont(io, title_font_size, {"C:/Windows/Fonts/seguisb.ttf", "C:/Windows/Fonts/segoeuib.ttf", "/System/Library/Fonts/SFNS.ttf", "/Library/Fonts/Inter-SemiBold.ttf", "/System/Library/Fonts/Supplemental/Helvetica Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"}, ui_ranges);
	g_font_mono = TryLoadFont(io, mono_font_size, {"C:/Windows/Fonts/consola.ttf", "C:/Windows/Fonts/cascadiamono.ttf", "C:/Windows/Fonts/CascadiaCode.ttf", "/Library/Fonts/JetBrainsMono-Regular.ttf", "/System/Library/Fonts/SFNSMono.ttf", "/System/Library/Fonts/Menlo.ttc", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf"}, terminal_ranges);

	if (g_font_mono == nullptr)
	{
		g_font_mono = g_font_ui;
	}

	if (PlatformServicesFactory::Instance().ui_traits.UseWindowsLayoutAdjustments() && g_font_mono != nullptr)
	{
		const std::initializer_list<const char*> lFontFallbacks = {
		    "C:/Windows/Fonts/seguisym.ttf", "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/meiryo.ttc", "C:/Windows/Fonts/malgun.ttf", "C:/Windows/Fonts/simsun.ttc",
		};
		MergeFontIntoLast(io, mono_font_size, lFontFallbacks, terminal_ranges);
	}

	if (g_font_ui != nullptr)
	{
		io.FontDefault = g_font_ui;
	}
}
