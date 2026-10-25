// HangWatchdog: the per-thread activity table and the sampler's stuck-thread decision, driven
// with a fake snapshot source and an explicit clock so it needs no real guest thread or timing.
#include <chrono>
#include <string>
#include <vector>

#include "check.h"
#include "zb/hang_watchdog.h"
#include "zb/runtime_report.h"

namespace {

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

}  // namespace

int main() {
    // record_thread_activity / snapshot_thread_activity: a fresh tid claims a slot, repeated
    // calls bump the counter and update the recorded activity in place.
    zb::record_thread_activity(9001, zb::ThreadActivityKind::kSyscall, 3);
    zb::record_thread_activity(9001, zb::ThreadActivityKind::kSyscall, 3);
    zb::record_thread_activity(9001, zb::ThreadActivityKind::kHostCall, 0x2a);
    {
        const auto snapshot = zb::snapshot_thread_activity();
        bool found = false;
        for (const auto& sample : snapshot) {
            if (sample.tid != 9001) continue;
            found = true;
            CHECK(sample.kind == zb::ThreadActivityKind::kHostCall);
            CHECK(sample.id == 0x2a);
            CHECK(sample.counter == 3);
            // Recorded calls start as in progress; the marker says so until they finish.
            CHECK(zb::describe_thread_activity(sample) == "host:0x2a(inside)");
            zb::record_thread_activity_done(9001);
            auto after = zb::snapshot_thread_activity();
            for (const auto& done : after) {
                if (done.tid == 9001) CHECK(zb::describe_thread_activity(done) == "host:0x2a");
            }
        }
        CHECK(found);
    }

    // HangWatchdog::sample: a thread whose activity never changes across kStuckThreshold + 1
    // samples is reported as stuck; one that keeps moving never is.
    zb::runtime_report().clear();
    using Clock = zb::HangWatchdog::Clock;
    Clock::time_point now{};
    bool stuck_activity = true;  // toggled by the fake snapshot to simulate the moving thread

    zb::HangWatchdog watchdog([&]() {
        std::vector<zb::ThreadActivitySample> out;
        zb::ThreadActivitySample stuck;
        stuck.tid = 111;
        stuck.kind = zb::ThreadActivityKind::kSyscall;
        stuck.id = 7;
        stuck.counter = 42;  // never changes
        out.push_back(stuck);

        zb::ThreadActivitySample moving;
        moving.tid = 222;
        moving.kind = zb::ThreadActivityKind::kHostCall;
        moving.id = 5;
        static std::uint64_t moving_counter = 0;
        if (stuck_activity) ++moving_counter;
        moving.counter = moving_counter;
        out.push_back(moving);
        return out;
    });

    // Sample 1: establishes the baseline for both threads; nothing is stuck yet.
    CHECK(!watchdog.sample(now));
    now += std::chrono::seconds(3);
    // Sample 2: thread 111 unchanged once, thread 222 still moving; below the threshold.
    CHECK(!watchdog.sample(now));
    now += std::chrono::seconds(3);
    // Sample 3: thread 111 unchanged twice in a row -> stuck. Thread 222 keeps moving.
    CHECK(watchdog.sample(now));

    const std::string text = zb::runtime_report().text();
    CHECK(contains(text, "watch-1: 111=sys:"));
    CHECK(contains(text, "x42"));
    CHECK(contains(text, "stuck=6"));
    CHECK(!contains(text, "222=host:"));

    // Once a thread starts moving again it drops out of the stuck list; if nothing new is stuck,
    // no further note is written.
    zb::runtime_report().clear();
    zb::HangWatchdog watchdog2([&]() {
        std::vector<zb::ThreadActivitySample> out;
        zb::ThreadActivitySample sample;
        sample.tid = 333;
        sample.kind = zb::ThreadActivityKind::kSyscall;
        sample.id = 1;
        static std::uint64_t counter = 0;
        sample.counter = ++counter;  // always moving
        out.push_back(sample);
        return out;
    });
    Clock::time_point now2{};
    CHECK(!watchdog2.sample(now2));
    now2 += std::chrono::seconds(3);
    CHECK(!watchdog2.sample(now2));
    now2 += std::chrono::seconds(3);
    CHECK(!watchdog2.sample(now2));
    CHECK(zb::runtime_report().text().find("watch-1") == std::string::npos);

    // classify_thread: idle waits are told apart from states that can be a hang.
    {
        zb::ThreadActivitySample s;
        s.kind = zb::ThreadActivityKind::kHostCall;
        s.id = 0xFE00;
        // Inside READY but not parked: running a command, judged like any other thread.
        s.in_progress = true;
        CHECK(zb::classify_thread(s, false, 0, false, false, false) == zb::ThreadState::kStalled);
        // Parked by the runtime: idle, whatever the last recorded call was.
        s.parked = true;
        CHECK(zb::classify_thread(s, false, 0, false, false, false) == zb::ThreadState::kIdleService);
        s.kind = zb::ThreadActivityKind::kSyscall;
        s.id = 126;  // rt_sigprocmask from the last command
        s.in_progress = false;
        CHECK(zb::classify_thread(s, false, 0, false, false, false) == zb::ThreadState::kIdleService);
        s.parked = false;
        s.kind = zb::ThreadActivityKind::kSyscall;
        s.id = 240;
        s.in_progress = true;
        CHECK(zb::classify_thread(s, false, 0, true, false, false) == zb::ThreadState::kIdleWait);
        CHECK(zb::classify_thread(s, false, 0, true, true, false) == zb::ThreadState::kLostWakeSuspect);
        CHECK(zb::classify_thread(s, false, 0, true, false, true) == zb::ThreadState::kLostWakeSuspect);
        CHECK(zb::classify_thread(s, true, 5, false, false, false) == zb::ThreadState::kProgressing);
        s.in_progress = false;
        CHECK(zb::classify_thread(s, false, 7, false, false, false) == zb::ThreadState::kSpinning);
        CHECK(zb::classify_thread(s, false, 0, false, false, false) == zb::ThreadState::kStalled);
    }

    // An idle service thread never raises a stuck note (it would use up the note budget long
    // before a real hang), but it still gets a rolling live line.
    zb::runtime_report().clear();
    zb::HangWatchdog idle_watchdog([&]() {
        std::vector<zb::ThreadActivitySample> out;
        zb::ThreadActivitySample service;
        service.tid = 444;
        service.kind = zb::ThreadActivityKind::kHostCall;
        service.id = 0xFE00;
        service.counter = 9;
        service.in_progress = true;
        service.parked = true;
        out.push_back(service);
        return out;
    });
    Clock::time_point now3{};
    for (int i = 0; i < 5; ++i) {
        CHECK(!idle_watchdog.sample(now3));
        now3 += std::chrono::seconds(3);
    }
    {
        const std::string idle_text = zb::runtime_report().text();
        CHECK(!contains(idle_text, "watch-1:"));
        CHECK(contains(idle_text, "watch-live-444: idle-service"));
    }

    // The live line records where the thread last left translated code, with recent distinct PCs.
    zb::runtime_report().clear();
    std::uint32_t pc = 0x1000;
    zb::HangWatchdog live_watchdog([&]() {
        std::vector<zb::ThreadActivitySample> out;
        zb::ThreadActivitySample busy;
        busy.tid = 555;
        busy.kind = zb::ThreadActivityKind::kSyscall;
        busy.id = 3;
        static std::uint64_t counter = 0;
        busy.counter = ++counter;
        busy.pc = pc;
        busy.lr = 0x2000;
        out.push_back(busy);
        return out;
    });
    Clock::time_point now4{};
    live_watchdog.sample(now4);
    pc = 0x1100;
    now4 += std::chrono::seconds(3);
    live_watchdog.sample(now4);
    {
        const std::string live_text = zb::runtime_report().text();
        CHECK(contains(live_text, "watch-live-555: progressing calls+1"));
        CHECK(contains(live_text, "pc=0x1100 lr=0x2000"));
        CHECK(contains(live_text, "recent-pc=[0x1000, 0x1100]"));
    }

    // A native method entered but not returned is listed.
    {
        zb::RuntimeReport report;
        zb::NativeCallCounter& key = report.native_call_counter("com/x/A.enqueueKey");
        key.count.fetch_add(2);
        key.returned.fetch_add(1);
        CHECK(contains(report.text(), "native-inside: com/x/A.enqueueKey=1"));
        key.returned.fetch_add(1);
        CHECK(!contains(report.text(), "native-inside:"));
    }

    // Capture on request: nothing until the watchdog asks, then the thread's own copy, once.
    {
        zb::record_thread_activity(777, zb::ThreadActivityKind::kSyscall, 78);
        CHECK(!zb::thread_capture_requested(777));
        std::uint32_t got[zb::kCaptureWords];
        std::uint32_t got_count = 0;
        CHECK(!zb::latest_thread_capture(777, got, got_count));
        zb::request_thread_capture(777);
        CHECK(zb::thread_capture_requested(777));
        const std::uint32_t words[] = {4, 5, 6, 7, 8, 9, 10, 11, 0x1000, 0x2000};
        zb::record_thread_capture(777, words, 10);
        CHECK(!zb::thread_capture_requested(777));
        CHECK(zb::latest_thread_capture(777, got, got_count));
        CHECK(got_count == 10);
        CHECK(got[0] == 4 && got[9] == 0x2000);
    }

    std::puts("hang_watchdog_test: ok");
    return 0;
}
