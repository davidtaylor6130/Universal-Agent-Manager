#include "terminal_text_utils.h"

#include <algorithm>

namespace uam {
namespace {

bool IsBeforeOrEqual(const TerminalSelectionPoint& lhs, const TerminalSelectionPoint& rhs) {
  return (lhs.row < rhs.row) || (lhs.row == rhs.row && lhs.col <= rhs.col);
}

void TrimTrailingSpaces(std::string& line) {
  while (!line.empty() && line.back() == ' ') {
    line.pop_back();
  }
}

}  // namespace

int ClampCliTerminalColumns(const int visible_cols, const int max_cols) {
  const int safe_visible = std::max(20, visible_cols);
  const int safe_max = std::clamp(max_cols, 40, 512);
  return std::min(safe_visible, safe_max);
}

bool NormalizeTerminalSelectionRange(const TerminalSelectionPoint& lhs,
                                     const TerminalSelectionPoint& rhs,
                                     TerminalSelectionPoint* start_out,
                                     TerminalSelectionPoint* end_out) {
  if (start_out == nullptr || end_out == nullptr) {
    return false;
  }
  if (IsBeforeOrEqual(lhs, rhs)) {
    *start_out = lhs;
    *end_out = rhs;
  } else {
    *start_out = rhs;
    *end_out = lhs;
  }
  return true;
}

bool TerminalSelectionContainsCell(const TerminalSelectionPoint& start,
                                   const TerminalSelectionPoint& end,
                                   const int row,
                                   const int col) {
  if (row < start.row || row > end.row) {
    return false;
  }
  if (start.row == end.row) {
    return row == start.row && col >= start.col && col <= end.col;
  }
  if (row == start.row) {
    return col >= start.col;
  }
  if (row == end.row) {
    return col <= end.col;
  }
  return true;
}

std::string ExtractTerminalSelectionText(const std::vector<std::vector<TerminalTextCell>>& rows,
                                         const TerminalSelectionPoint& lhs,
                                         const TerminalSelectionPoint& rhs) {
  TerminalSelectionPoint start{};
  TerminalSelectionPoint end{};
  if (!NormalizeTerminalSelectionRange(lhs, rhs, &start, &end)) {
    return "";
  }
  if (rows.empty() || start.row < 0 || end.row < 0 || start.row >= static_cast<int>(rows.size())) {
    return "";
  }

  const int clamped_end_row = std::min(end.row, static_cast<int>(rows.size()) - 1);
  std::string out;
  for (int row = start.row; row <= clamped_end_row; ++row) {
    const std::vector<TerminalTextCell>& cells = rows[static_cast<std::size_t>(row)];
    const int row_start_col = (row == start.row) ? start.col : 0;
    const int row_end_col = (row == end.row) ? end.col : static_cast<int>(cells.size()) - 1;
    if (row_start_col <= row_end_col && !cells.empty()) {
      std::string line;
      for (int col = std::max(0, row_start_col); col <= row_end_col && col < static_cast<int>(cells.size()); ++col) {
        const TerminalTextCell& cell = cells[static_cast<std::size_t>(col)];
        if (cell.width == 0) {
          continue;
        }
        if (cell.text.empty()) {
          line.push_back(' ');
        } else {
          line += cell.text;
        }
      }
      TrimTrailingSpaces(line);
      out += line;
    }
    if (row < clamped_end_row) {
      out.push_back('\n');
    }
  }
  return out;
}

}  // namespace uam
