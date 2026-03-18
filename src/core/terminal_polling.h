#pragma once

#include <cstddef>

namespace uam {

struct TerminalDrainBudget {
  static constexpr std::size_t kDefaultMaxReads = 32;
  static constexpr std::size_t kDefaultMaxBytes = 512 * 1024;
  static constexpr std::size_t kBackgroundMaxReads = 8;
  static constexpr std::size_t kBackgroundMaxBytes = 64 * 1024;

  std::size_t max_reads = kDefaultMaxReads;
  std::size_t max_bytes = kDefaultMaxBytes;
  std::size_t reads = 0;
  std::size_t bytes = 0;

  bool CanDrainMore() const {
    return reads < max_reads && bytes < max_bytes;
  }

  void RecordRead(const std::size_t byte_count) {
    ++reads;
    bytes += byte_count;
  }
};

inline TerminalDrainBudget TerminalDrainBudgetForView(const bool background_poll) {
  TerminalDrainBudget budget;
  if (background_poll) {
    budget.max_reads = TerminalDrainBudget::kBackgroundMaxReads;
    budget.max_bytes = TerminalDrainBudget::kBackgroundMaxBytes;
  }
  return budget;
}

inline bool ShouldSyncNativeHistoryAfterTerminalPoll(const bool terminal_running) {
  // Keep terminal polling responsive by syncing native history only after the
  // terminal session stops.
  return !terminal_running;
}

inline double HiddenTerminalPollIntervalSeconds() {
  // Background terminals still need to drain output, but not every frame.
  return 0.05;
}

inline bool ShouldPollBackgroundTerminalNow(const double now_s,
                                            const double last_poll_s,
                                            const double min_interval_s = HiddenTerminalPollIntervalSeconds()) {
  if (last_poll_s <= 0.0) {
    return true;
  }
  return (now_s - last_poll_s) >= min_interval_s;
}

}  // namespace uam
