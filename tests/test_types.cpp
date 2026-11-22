#include <gtest/gtest.h>

#include "tickbook/types.hpp"

TEST(Event, InitializationZerosFlagsAndExplicitPadding) {
  tickbook::Event default_event;
  EXPECT_EQ(default_event.flags, 0);
  for (const auto byte : default_event.padding) EXPECT_EQ(byte, 0);

  tickbook::Event event{123, 456, 789, 100, 3, tickbook::EventType::BookUpdate,
                        tickbook::Side::Bid};

  EXPECT_EQ(event.flags, 0);
  for (const auto byte : event.padding) EXPECT_EQ(byte, 0);

  event.flags = tickbook::kEventFlagEndOfMessage;
  EXPECT_EQ(event.flags & tickbook::kEventFlagEndOfMessage,
            tickbook::kEventFlagEndOfMessage);
}
