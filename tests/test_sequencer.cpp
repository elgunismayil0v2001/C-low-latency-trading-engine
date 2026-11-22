#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>

#include "tickbook/sequencer.hpp"
#include "tickbook/types.hpp"

using tickbook::DiffSequencer;
using tickbook::SeqAction;

TEST(DiffSequencer, DropsEventsOlderThanSnapshot) {
  DiffSequencer s(100);
  EXPECT_EQ(s.on_message(90, 100), SeqAction::Drop);
  EXPECT_EQ(s.on_message(95, 99), SeqAction::Drop);
  EXPECT_FALSE(s.synced());
}

TEST(DiffSequencer, FirstEventMustSpanSnapshot) {
  DiffSequencer s(100);
  EXPECT_EQ(s.on_message(98, 105), SeqAction::Apply); // U <= 101 <= u
  EXPECT_TRUE(s.synced());
}

TEST(DiffSequencer, FirstEventAfterSnapshotGapRequestsResync) {
  DiffSequencer s(100);
  EXPECT_EQ(s.on_message(105, 110), SeqAction::Resync); // U > snapshot + 1
}

TEST(DiffSequencer, ContiguousEventsApply) {
  DiffSequencer s(100);
  EXPECT_EQ(s.on_message(101, 105), SeqAction::Apply);
  EXPECT_EQ(s.on_message(106, 110), SeqAction::Apply);
  EXPECT_EQ(s.on_message(111, 111), SeqAction::Apply);
}

TEST(DiffSequencer, GapAfterSyncRequestsResync) {
  DiffSequencer s(100);
  EXPECT_EQ(s.on_message(101, 105), SeqAction::Apply);
  EXPECT_EQ(s.on_message(107, 110), SeqAction::Resync);
}

TEST(DiffSequencer, DuplicateAfterSyncIsDropped) {
  DiffSequencer s(100);
  EXPECT_EQ(s.on_message(101, 105), SeqAction::Apply);
  EXPECT_EQ(s.on_message(101, 105), SeqAction::Drop);
  EXPECT_EQ(s.on_message(106, 110), SeqAction::Apply);
}

TEST(DiffSequencer, SequencesMultiLevelMessageOnce) {
  DiffSequencer s(100);
  const std::array<tickbook::Event, 3> levels{{
      {0, 101, 105, 100, 1, tickbook::EventType::BookUpdate, tickbook::Side::Bid},
      {0, 101, 105, 99, 2, tickbook::EventType::BookUpdate, tickbook::Side::Bid},
      {0, 101, 105, 102, 3, tickbook::EventType::BookUpdate, tickbook::Side::Ask},
  }};

  // Sequence the complete Binance message once, then apply every flattened level.
  const auto action = s.on_message(levels.front().first_id, levels.front().last_id);
  EXPECT_EQ(action, SeqAction::Apply);
  std::size_t applied_levels = 0;
  if (action == SeqAction::Apply) {
    for (const auto& level : levels) {
      EXPECT_EQ(level.first_id, levels.front().first_id);
      EXPECT_EQ(level.last_id, levels.front().last_id);
      ++applied_levels;
    }
  }
  EXPECT_EQ(applied_levels, levels.size());
  EXPECT_EQ(s.on_message(106, 110), SeqAction::Apply);
}

TEST(DiffSequencer, EmptyMessageStillAdvancesSequence) {
  DiffSequencer s(100);
  EXPECT_EQ(s.on_message(101, 105), SeqAction::Apply);

  // No flattened Event exists for this message, but its U/u still advances the sequence.
  constexpr std::array<tickbook::Event, 0> no_levels{};
  EXPECT_TRUE(no_levels.empty());
  EXPECT_EQ(s.on_message(106, 110), SeqAction::Apply);
  EXPECT_EQ(s.on_message(111, 115), SeqAction::Apply);

  // An empty message still carries update IDs, so a gap in it must request a resync.
  DiffSequencer gap(100);
  EXPECT_EQ(gap.on_message(101, 105), SeqAction::Apply);
  EXPECT_TRUE(no_levels.empty());
  EXPECT_EQ(gap.on_message(107, 110), SeqAction::Resync);
}

TEST(DiffSequencer, OverlappingUpdateRangesApply) {
  DiffSequencer s(100);
  EXPECT_EQ(s.on_message(101, 105), SeqAction::Apply);
  EXPECT_EQ(s.on_message(104, 108), SeqAction::Apply);
  EXPECT_EQ(s.on_message(109, 110), SeqAction::Apply);
}

TEST(DiffSequencer, CanResetAfterResyncWithFreshSnapshot) {
  DiffSequencer s(100);
  EXPECT_EQ(s.on_message(101, 105), SeqAction::Apply);
  EXPECT_EQ(s.on_message(107, 110), SeqAction::Resync);

  s.reset(110);
  EXPECT_FALSE(s.synced());
  EXPECT_EQ(s.on_message(108, 110), SeqAction::Drop);
  EXPECT_EQ(s.on_message(109, 115), SeqAction::Apply);
  EXPECT_TRUE(s.synced());
}

TEST(DiffSequencer, UpdateIdsAtUint64MaxDoNotOverflow) {
  constexpr auto max_id = std::numeric_limits<std::uint64_t>::max();

  DiffSequencer snapshot_at_max(max_id);
  EXPECT_EQ(snapshot_at_max.on_message(max_id, max_id), SeqAction::Drop);
  EXPECT_FALSE(snapshot_at_max.synced());

  DiffSequencer reaches_max(max_id - 1);
  EXPECT_EQ(reaches_max.on_message(max_id, max_id), SeqAction::Apply);
  EXPECT_EQ(reaches_max.on_message(max_id, max_id), SeqAction::Drop);
}
