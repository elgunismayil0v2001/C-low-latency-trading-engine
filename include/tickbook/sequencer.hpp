#pragma once
#include <cstdint>

namespace tickbook {

enum class SeqAction { Drop, Apply, Resync };

// Binance diff-depth synchronisation, as described in the Binance spot docs at the
// time of writing: buffer events, fetch a REST snapshot, drop events with u <= lastUpdateId,
// the first applied event must satisfy U <= lastUpdateId + 1 <= u, and afterwards every
// event must have U == previous u + 1. ALWAYS re-check this against the current docs.
class DiffSequencer {
public:
  explicit DiffSequencer(std::uint64_t snapshot_last_update_id) noexcept : snap_id_(snapshot_last_update_id) {}

  [[nodiscard]] SeqAction on_message(std::uint64_t first_id, std::uint64_t last_id) noexcept {
    if (!synced_) {
      if (last_id <= snap_id_) return SeqAction::Drop;
      if (first_id <= snap_id_ + 1 && snap_id_ + 1 <= last_id) {
        synced_ = true;
        prev_last_ = last_id;
        return SeqAction::Apply;
      }
      return SeqAction::Resync; // first usable event starts after the snapshot: a gap
    }
    if (first_id != prev_last_ + 1) return SeqAction::Resync; // gap or duplicate
    prev_last_ = last_id;
    return SeqAction::Apply;
  }

  [[nodiscard]] bool synced() const noexcept { return synced_; }

private:
  std::uint64_t snap_id_;
  std::uint64_t prev_last_{0};
  bool synced_{false};
};

} // namespace tickbook
