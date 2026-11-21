#include <gtest/gtest.h>

#include <cstdint>
#include <thread>

#include "tickbook/ring_buffer.hpp"

using tickbook::SpscRing;

TEST(SpscRing, PushPopKeepsOrder) {
  SpscRing<int, 8> q;
  int out = 0;
  EXPECT_FALSE(q.try_pop(out));
  for (int i = 0; i < 5; ++i) EXPECT_TRUE(q.try_push(i));
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, i);
  }
  EXPECT_FALSE(q.try_pop(out));
}

TEST(SpscRing, ReportsFullAndRecovers) {
  SpscRing<int, 4> q;
  for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.try_push(i));
  EXPECT_FALSE(q.try_push(99));
  int out = 0;
  EXPECT_TRUE(q.try_pop(out));
  EXPECT_EQ(out, 0);
  EXPECT_TRUE(q.try_push(99));
}

TEST(SpscRing, WrapsAroundManyTimes) {
  SpscRing<std::uint64_t, 4> q;
  std::uint64_t out = 0;
  for (std::uint64_t i = 0; i < 1000; ++i) {
    EXPECT_TRUE(q.try_push(i));
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, i);
  }
}

TEST(SpscRing, TwoThreadsKeepOrderAndLoseNothing) {
  constexpr std::uint64_t kCount = 200000;
  SpscRing<std::uint64_t, 1024> q;
  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kCount; ++i) {
      while (!q.try_push(i)) std::this_thread::yield();
    }
  });
  std::uint64_t expected = 0;
  bool in_order = true;
  while (expected < kCount) {
    std::uint64_t v = 0;
    if (q.try_pop(v)) {
      if (v != expected) in_order = false;
      ++expected;
    } else {
      std::this_thread::yield();
    }
  }
  producer.join();
  EXPECT_TRUE(in_order);
  EXPECT_EQ(expected, kCount);
}
