#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace zb {

// Hang diagnostics: a guest that stops making progress (gray screen, no crash) leaves nothing
// in the crash report, because nothing crashed. This records, per guest thread, the last thing
// it did on its way out of translated code, and a background thread periodically checks whether
// that has changed.

// The two places a guest thread leaves translated code.
enum class ThreadActivityKind : std::uint8_t {
    kNone = 0,
    kSyscall = 1,
    kHostCall = 2,
};

// Records that guest thread `tid` just performed a syscall (`id` = syscall number) or a host
// call (`id` = host-call index). Lock-free, allocation-free, safe from any thread: called on
// every syscall and every host call, so it must stay cheap.
// `pc`/`lr` are the guest registers at that exit (0 when unknown): where in the guest the thread
// last left translated code, which is all that can be read safely from another thread later.
void record_thread_activity(std::int32_t tid, ThreadActivityKind kind, std::uint32_t id,
                            std::uint32_t pc = 0, std::uint32_t lr = 0);

// The host call recorded last for this thread has returned. A thread whose last recorded event is
// still in progress is blocked inside our own code, which reads very differently from a guest
// that simply stopped calling out.
void record_thread_activity_done(std::int32_t tid);

// The library runtime marks a thread while it sleeps for work in its own wait (the zbhost service
// queue, an unleased carrier). This, and not the last call recorded, is what makes a thread
// idle-service: a service thread running a command, or a leased carrier whose identity a borrower
// is using, is not marked and is judged by what it does.
void set_thread_parked(std::int32_t tid, bool parked);

// Register and stack capture on request. The watchdog asks for one per busy thread; the thread
// itself copies r4-r11 and its nearest stack words at its next syscall, so no thread's CPU state
// is ever read from another thread.
constexpr std::size_t kCaptureWords = 8 + 64;  // r4-r11, then stack words from sp up
// Cheap check made on every syscall: has the watchdog asked this thread for a capture?
bool thread_capture_requested(std::int32_t tid);
void record_thread_capture(std::int32_t tid, const std::uint32_t* words, std::uint32_t count);
// The thread's latest capture; false when it has none.
bool latest_thread_capture(std::int32_t tid, std::uint32_t* words, std::uint32_t& count);
void request_thread_capture(std::int32_t tid);

struct ThreadActivitySample {
    std::int32_t tid = 0;
    ThreadActivityKind kind = ThreadActivityKind::kNone;
    std::uint32_t id = 0;
    std::uint64_t counter = 0;
    bool in_progress = false;  // still inside the recorded host call or syscall
    std::uint32_t pc = 0;
    std::uint32_t lr = 0;
    bool parked = false;  // set_thread_parked
};

// What a thread is doing between two samples. Waits that are part of normal operation (a
// condition variable nobody has signalled, the library runtime's service and carrier parking)
// are told apart from the states that can be a hang, so that idle threads neither raise stuck
// notes nor use up the note budget before the real hang happens.
enum class ThreadState : std::uint8_t {
    kProgressing,      // left translated code again since the last sample
    kIdleService,      // asleep in the library runtime's own wait for work (set_thread_parked)
    kIdleWait,         // in FUTEX_WAIT, the word unchanged and no wake issued on it since
    kLostWakeSuspect,  // in FUTEX_WAIT, but the word changed or a wake on it woke nobody
    kSpinning,         // no exit from translated code, CPU still climbing
    kStalled,          // no exit, no CPU: blocked in another call, or parked
};

const char* thread_state_name(ThreadState state);

// The classification HangWatchdog uses. `changed`: the activity counter moved since the last
// sample. `cpu_delta`: CPU ticks burned since then. `waiting`, `word_changed`, `wake_missed`
// describe the thread's current FUTEX_WAIT from futex_trace (waiting false when it is in none).
ThreadState classify_thread(const ThreadActivitySample& sample, bool changed, std::uint64_t cpu_delta,
                            bool waiting, bool word_changed, bool wake_missed);

// A snapshot of every guest thread slot touched so far.
std::vector<ThreadActivitySample> snapshot_thread_activity();

// "sys:openat" or "host:0x2a3"; "(none)" for a slot that was never recorded.
std::string describe_thread_activity(const ThreadActivitySample& sample);

// Periodically samples snapshot_thread_activity() and records a report note when one or more
// threads have made no progress since the previous two samples. Never aborts or kills the
// process; this is diagnostics only.
class HangWatchdog {
public:
    using Clock = std::chrono::steady_clock;
    using SnapshotFn = std::function<std::vector<ThreadActivitySample>()>;

    static constexpr std::chrono::seconds kInterval{3};
    static constexpr int kStuckThreshold = 2;      // consecutive unchanged samples
    static constexpr std::size_t kMaxNotes = 6;    // report lines this watchdog will ever write
    static constexpr std::size_t kMaxThreadsShown = 8;

    explicit HangWatchdog(SnapshotFn snapshot = snapshot_thread_activity);

    // One sampling pass at `now`. Returns true if it wrote a new watch-<n> report note.
    bool sample(Clock::time_point now);

    // Runs sample() every kInterval, forever, on the calling thread.
    [[noreturn]] void run_forever();

private:
    struct State {
        std::int32_t tid = 0;
        ThreadActivityKind kind = ThreadActivityKind::kNone;
        std::uint32_t id = 0;
        std::uint64_t counter = 0;
        Clock::time_point since{};  // when this (kind, id, counter) was first observed
        int unchanged_samples = 0;
        std::uint64_t cpu = 0;      // CPU ticks at this sample
        std::uint64_t calls = 0;    // counter change since the previous sample
        ThreadState state = ThreadState::kProgressing;
        std::vector<std::uint32_t> pcs;  // the last few distinct exit PCs, oldest first
        std::vector<std::uint32_t> m68k_pcs;  // StyleTap only: last few distinct 68K PCs
    };
    static constexpr std::size_t kMaxLiveThreads = 8;  // threads with a rolling live-<tid> line
    std::set<std::int32_t> live_tids_;
    std::size_t live_threads_ = 0;

    SnapshotFn snapshot_;
    std::vector<State> previous_;
    std::size_t notes_written_ = 0;
};

// Starts the watchdog on a detached background thread, once per process. GuestJniEngine calls
// this when the runtime starts; the runtime is process-lifetime, so there is no shutdown path.
void start_hang_watchdog();

}  // namespace zb
