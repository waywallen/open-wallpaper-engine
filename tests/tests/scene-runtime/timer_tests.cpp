#include <rstd/test/gtest.hpp>

import rstd;
import wescene.timer;

using namespace rstd::prelude;
using rstd::sync::atomic::Atomic;
using rstd::time::Duration;
using rstd::time::Instant;

namespace
{
bool WaitFor(const Atomic<u32>& count, u32 target) {
    auto start = Instant::now();
    while (count.load() < target && start.elapsed() < Duration::from_secs(u64(3)))
        rstd::thread::sleep(Duration::from_millis(u64(1)));
    return count.load() >= target;
}
} // namespace

TEST(ThreadTimer, StopsWithoutEmittingAndCanRestart) {
    Atomic<u32>      calls {};
    owe::ThreadTimer timer(owe::ThreadTimer::Callback::make([&] {
        calls.fetch_add(u32(1));
    }));
    EXPECT_FALSE(timer.Running());
    timer.Stop();
    timer.SetInterval(Duration::from_secs(u64(30)));
    auto started = Instant::now();
    timer.Start();
    timer.Start();
    timer.Stop();
    EXPECT_TRUE(started.elapsed() < Duration::from_secs(u64(3)));
    EXPECT_EQ(calls.load(), u32());
    timer.SetInterval(Duration::from_millis(u64(1)));
    timer.Start();
    EXPECT_TRUE(WaitFor(calls, u32(2)));
    timer.Stop();
    auto stopped = calls.load();
    rstd::thread::sleep(Duration::from_millis(u64(10)));
    EXPECT_EQ(calls.load(), stopped);
    EXPECT_FALSE(timer.Running());
}

TEST(FrameTimer, OwnsMutableCallbackAndPreservesBackpressure) {
    Atomic<u32>     calls {};
    auto            state = Box<u32>::make();
    owe::FrameTimer timer(
        Some(owe::FrameTimer::Callback::make([state = rstd::move(state), &calls]() mutable {
            calls.store(++*state);
        })));
    timer.SetRequiredFps(u16(1000));
    timer.Run();
    ASSERT_TRUE(WaitFor(calls, u32(4)));
    rstd::thread::sleep(Duration::from_millis(u64(10)));
    EXPECT_EQ(calls.load(), u32(4));
    timer.SetCallback(None());
    timer.FrameBegin();
    timer.FrameEnd();
    EXPECT_TRUE(WaitFor(calls, u32(5)));
    timer.Stop();
    timer.SetCallback(None());
    auto stopped = calls.load();
    timer.Run();
    rstd::thread::sleep(Duration::from_millis(u64(10)));
    timer.Stop();
    EXPECT_EQ(calls.load(), stopped);
}

TEST(FrameTimer, SerializesSamplesWithFpsChanges) {
    owe::FrameTimer timer;
    EXPECT_EQ(timer.RequiredFps(), u16(15));
    EXPECT_DOUBLE_EQ(timer.TargetFrameTime(), 0.066666);
    timer.SetRequiredFps(u16());
    EXPECT_EQ(timer.RequiredFps(), u16(1));
    EXPECT_DOUBLE_EQ(timer.FrameTime(), 1.0);
    auto writer = rstd::thread::spawn([&] {
                      for (int i = 0; i < 100; ++i) timer.SetRequiredFps(u16(30 + i));
                  }).unwrap();
    for (int i = 0; i < 100; ++i) {
        timer.FrameBegin();
        timer.FrameEnd();
    }
    rstd::move(writer).join().unwrap();
    EXPECT_EQ(timer.RequiredFps(), u16(129));
    EXPECT_TRUE(timer.TargetFrameTime() > 0.0);
}
