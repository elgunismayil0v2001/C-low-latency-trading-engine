#include <gtest/gtest.h>

#include "tickbook/sequencer.hpp"

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

TEST(DiffSequencer, DuplicateAfterSyncRequestsResync) {
  DiffSequencer s(100);
  EXPECT_EQ(s.on_message(101, 105), SeqAction::Apply);
  EXPECT_EQ(s.on_message(101, 105), SeqAction::Resync);
}
