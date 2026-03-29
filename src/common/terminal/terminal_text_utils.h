#pragma once

#include <string>
#include <vector>

namespace uam {

struct TerminalSelectionPoint {
  int row = 0;
  int col = 0;
};

struct TerminalTextCell {
  std::string text;
  int width = 1;
};

int ClampCliTerminalColumns(int visible_cols, int max_cols);

bool NormalizeTerminalSelectionRange(const TerminalSelectionPoint& lhs,
                                     const TerminalSelectionPoint& rhs,
                                     TerminalSelectionPoint* start_out,
                                     TerminalSelectionPoint* end_out);

bool TerminalSelectionContainsCell(const TerminalSelectionPoint& start,
                                   const TerminalSelectionPoint& end,
                                   int row,
                                   int col);

std::string ExtractTerminalSelectionText(const std::vector<std::vector<TerminalTextCell>>& rows,
                                         const TerminalSelectionPoint& lhs,
                                         const TerminalSelectionPoint& rhs);

}  // namespace uam
