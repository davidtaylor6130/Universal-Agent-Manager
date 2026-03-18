#pragma once

#include <string>
#include <vector>

namespace uam {

struct TerminalTypographyConfig {
  float mono_font_size = 13.5f;
  float cell_width_scale = 1.0f;
  std::vector<const char*> mono_font_candidates;
  std::vector<const char*> readable_mono_font_candidates;
  std::vector<const char*> fallback_font_candidates;
};

inline const TerminalTypographyConfig& TerminalTypographyConfigForPlatform() {
  static const TerminalTypographyConfig config = [] {
#if defined(_WIN32)
    return TerminalTypographyConfig{
        14.0f,
        1.10f,
        {
            "C:/Windows/Fonts/cascadiamono.ttf",
            "C:/Windows/Fonts/consola.ttf",
            "C:/Windows/Fonts/cour.ttf",
        },
        {
            "C:/Windows/Fonts/consolab.ttf",
            "C:/Windows/Fonts/cascadiamono.ttf",
            "C:/Windows/Fonts/consola.ttf",
        },
        {
            "C:/Windows/Fonts/segoeui.ttf",
            "C:/Windows/Fonts/seguisym.ttf",
            "C:/Windows/Fonts/msyh.ttc",
            "C:/Windows/Fonts/meiryo.ttc",
            "C:/Windows/Fonts/malgun.ttf",
            "C:/Windows/Fonts/seguiemj.ttf",
        },
    };
#else
    return TerminalTypographyConfig{
        13.5f,
        1.0f,
        {
            "/Library/Fonts/JetBrainsMono-Regular.ttf",
            "/Library/Fonts/Menlo.ttc",
            "/System/Library/Fonts/Menlo.ttc",
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        },
        {
            "/Library/Fonts/JetBrainsMono-Bold.ttf",
            "/Library/Fonts/Menlo.ttc",
            "/System/Library/Fonts/Menlo.ttc",
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationMono-Bold.ttf",
        },
        {
            "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
            "/System/Library/Fonts/PingFang.ttc",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/truetype/noto/NotoSansSymbols-Regular.ttf",
            "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",
        },
    };
#endif
  }();
  return config;
}

inline float TerminalCellWidthScale() {
  return TerminalTypographyConfigForPlatform().cell_width_scale;
}

inline float TerminalMonoFontSize() {
  return TerminalTypographyConfigForPlatform().mono_font_size;
}

}  // namespace uam
