#pragma once
#include <cstdint>

namespace tickbook {

enum class SeqAction { Drop, Apply, Resync };

// Binance diff-depth synchronisation. Call once for each complete WebSocket message,
// before flattening its price levels; this also sequences messages with empty level arrays.
// See https://github.com/binance/binance-spot-api-docs/blob/master/web-socket-streams.md
// for the snapshot bridge and update-ID gap rules.
class DiffSequencer {
public:
  explicit DiffSequencer(std::uint64_t snapshot_last_update_id) noexcept
      : snap_id_(snapshot_last_update_id) {}

  // Reinitialize from a newly fetched snapshot after a Resync.
  void reset(std::uint64_t snapshot_last_update_id) noexcept {
    snap_id_ = snapshot_last_update_id;
    prev_last_ = 0;
    synced_ = false;
  }

  // first_id and last_id are the U/u fields of one Binance message, not one flattened level.
  [[nodiscard]] SeqAction on_message(std::uint64_t first_id, std::uint64_t last_id) noexcept {
    if (!synced_) {
      if (last_id <= snap_id_) return SeqAction::Drop;
      const auto next_id = snap_id_ + 1; // Safe because last_id > snap_id_.
      if (first_id <= next_id && next_id <= last_id) {
        synced_ = true;
        prev_last_ = last_id;
        return SeqAction::Apply;
      }
      return SeqAction::Resync; // first usable event starts after the snapshot: a gap
    }
    if (first_id == prev_last_ + 1 && last_id > prev_last_) {
      prev_last_ = last_id;
      return SeqAction::Apply;
    }
    if (last_id <= prev_last_) return SeqAction::Drop; // stale or duplicate message
    const auto next_id = prev_last_ + 1;               // Safe because last_id > prev_last_.
    if (first_id > next_id) return SeqAction::Resync;
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
