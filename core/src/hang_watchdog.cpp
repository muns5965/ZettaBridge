// Hang watchdog: per-thread last-activity slots (lock-free, allocation-free) plus a background
// sampler that turns "same activity, same counter, two samples running" into a report note. No
// guest semantics change here; a stuck thread is only ever described, never touched.
#include "zb/hang_watchdog.h"

#include <dirent.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "zb/futex_trace.h"
#include "zb/guest_probe.h"
#include "zb/host_jni.h"
#include "zb/runtime_report.h"
#include "zb/syscalls.h"

namespace zb {

namespace {

// Fixed-size, never-grows table of per-thread activity slots. A guest thread claims a slot the
// first time it is seen and keeps it (tids are not reused across a run in a way that matters for
// diagnostics); a slot is identified by `tid` with 0 meaning free.
constexpr std::size_t kMaxSlots = 256;

struct Slot {
    std::atomic<bool> in_progress{false};
    std::atomic<std::int32_t> tid{0};
    std::atomic<std::uint8_t> kind{static_cast<std::uint8_t>(ThreadActivityKind::kNone)};
    std::atomic<std::uint32_t> id{0};
    std::atomic<std::uint64_t> counter{0};
    std::atomic<std::uint32_t> pc{0};
    std::atomic<std::uint32_t> lr{0};
    // Capture on request: seq odd while the owner writes.
    std::atomic<bool> capture_wanted{false};
    std::atomic<std::uint32_t> capture_seq{0};
    std::uint32_t capture_count = 0;
    std::uint32_t capture[kCaptureWords] = {};
};

Slot g_slots[kMaxSlots];

}  // namespace

void record_thread_activity(std::int32_t tid, ThreadActivityKind kind, std::uint32_t id, std::uint32_t pc,
                            std::uint32_t lr) {
    if (tid == 0) return;
    // This runs on every guest syscall and host call, so the common path must not scan the table:
    // each thread remembers its own slot after claiming it once.
    static thread_local Slot* mine = nullptr;
    static thread_local std::int32_t mine_tid = 0;
    if (mine != nullptr && mine_tid == tid) {
        mine->pc.store(pc, std::memory_order_relaxed);
        mine->lr.store(lr, std::memory_order_relaxed);
        mine->kind.store(static_cast<std::uint8_t>(kind), std::memory_order_relaxed);
        mine->id.store(id, std::memory_order_relaxed);
        mine->counter.fetch_add(1, std::memory_order_relaxed);
        mine->in_progress.store(true, std::memory_order_relaxed);
        return;
    }
    // Find an existing slot for this tid, or claim the first free one. A relaxed CAS race
    // between two threads claiming the same free slot for different tids is possible but
    // vanishingly rare and self-heals: the loser just tries the next slot next call.
    Slot* free_slot = nullptr;
    for (Slot& slot : g_slots) {
        const std::int32_t current = slot.tid.load(std::memory_order_relaxed);
        if (current == tid) {
            slot.kind.store(static_cast<std::uint8_t>(kind), std::memory_order_relaxed);
            slot.id.store(id, std::memory_order_relaxed);
            slot.counter.fetch_add(1, std::memory_order_relaxed);
            slot.in_progress.store(true, std::memory_order_relaxed);
            mine = &slot;
            mine_tid = tid;
            return;
        }
        if (current == 0 && free_slot == nullptr) free_slot = &slot;
    }
    if (free_slot == nullptr) return;  // table full; diagnostics only, never blocks the guest
    std::int32_t expected = 0;
    if (!free_slot->tid.compare_exchange_strong(expected, tid, std::memory_order_relaxed)) {
        // Lost the race; the winner's slot is not this tid's, try once more on the next call.
        return;
    }
    free_slot->kind.store(static_cast<std::uint8_t>(kind), std::memory_order_relaxed);
    free_slot->id.store(id, std::memory_order_relaxed);
    free_slot->counter.store(1, std::memory_order_relaxed);
    mine = free_slot;
    mine_tid = tid;
}

namespace {
Slot* find_slot(std::int32_t tid) {
    for (Slot& slot : g_slots) {
        if (slot.tid.load(std::memory_order_relaxed) == tid) return &slot;
    }
    return nullptr;
}
}  // namespace

bool thread_capture_requested(std::int32_t tid) {
    static thread_local Slot* cached = nullptr;
    static thread_local std::int32_t cached_tid = 0;
    if (cached == nullptr || cached_tid != tid || cached->tid.load(std::memory_order_relaxed) != tid) {
        cached = find_slot(tid);
        cached_tid = tid;
        if (cached == nullptr) return false;
    }
    return cached->capture_wanted.load(std::memory_order_relaxed);
}

void record_thread_capture(std::int32_t tid, const std::uint32_t* words, std::uint32_t count) {
    Slot* slot = find_slot(tid);
    if (slot == nullptr) return;
    if (count > kCaptureWords) count = kCaptureWords;
    slot->capture_seq.fetch_add(1, std::memory_order_acq_rel);
    for (std::uint32_t i = 0; i < count; ++i) slot->capture[i] = words[i];
    slot->capture_count = count;
    slot->capture_seq.fetch_add(1, std::memory_order_acq_rel);
    slot->capture_wanted.store(false, std::memory_order_relaxed);
}

bool latest_thread_capture(std::int32_t tid, std::uint32_t* words, std::uint32_t& count) {
    Slot* slot = find_slot(tid);
    if (slot == nullptr) return false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const std::uint32_t before = slot->capture_seq.load(std::memory_order_acquire);
        if (before == 0 || (before & 1)) return false;
        count = slot->capture_count;
        for (std::uint32_t i = 0; i < count; ++i) words[i] = slot->capture[i];
        if (slot->capture_seq.load(std::memory_order_acquire) == before) return true;
    }
    return false;
}

void request_thread_capture(std::int32_t tid) {
    if (Slot* slot = find_slot(tid)) slot->capture_wanted.store(true, std::memory_order_relaxed);
}

void record_thread_activity_done(std::int32_t tid) {
    for (Slot& slot : g_slots) {
        if (slot.tid.load(std::memory_order_relaxed) == tid) {
            slot.in_progress.store(false, std::memory_order_relaxed);
            return;
        }
    }
}

std::vector<ThreadActivitySample> snapshot_thread_activity() {
    std::vector<ThreadActivitySample> out;
    for (Slot& slot : g_slots) {
        const std::int32_t tid = slot.tid.load(std::memory_order_relaxed);
        if (tid == 0) continue;
        ThreadActivitySample sample;
        sample.tid = tid;
        sample.kind = static_cast<ThreadActivityKind>(slot.kind.load(std::memory_order_relaxed));
        sample.id = slot.id.load(std::memory_order_relaxed);
        sample.counter = slot.counter.load(std::memory_order_relaxed);
        sample.in_progress = slot.in_progress.load(std::memory_order_relaxed);
        sample.pc = slot.pc.load(std::memory_order_relaxed);
        sample.lr = slot.lr.load(std::memory_order_relaxed);
        out.push_back(sample);
    }
    return out;
}

// Ticks of CPU this thread has burned, from /proc/self/task/<tid>/stat, or 0 when unreadable.
// A thread whose activity never changes but whose CPU keeps climbing is spinning in translated
// code; one whose CPU stands still is blocked.
// What the kernel says about a thread: "gone" when it no longer exists, otherwise its scheduler
// state (R running, S sleeping, D uninterruptible), the CPU ticks it burned, and the kernel
// function it is waiting in. Zero CPU alone cannot tell an idle thread from a dead one.
std::string thread_kernel_state(std::int32_t tid) {
    char path[64];
    std::snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
    std::FILE* file = std::fopen(path, "re");
    if (file == nullptr) return "gone";
    char line[1024];
    const char* read = std::fgets(line, sizeof line, file);
    std::fclose(file);
    if (read == nullptr) return "gone";
    const char* cursor = std::strrchr(line, ')');
    if (cursor == nullptr) return "?";
    char state = '?';
    unsigned long long utime = 0, stime = 0;
    // Fields after "comm": state, ppid, pgrp, session, tty, tpgid, flags, min_flt, cmin_flt,
    // maj_flt, cmaj_flt, utime, stime.
    if (std::sscanf(cursor + 2, "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &state, &utime,
                    &stime) != 3) {
        return "?";
    }
    char waiting[64] = {};
    std::snprintf(path, sizeof path, "/proc/self/task/%d/wchan", tid);
    std::FILE* wchan = std::fopen(path, "re");
    if (wchan != nullptr) {
        if (std::fgets(waiting, sizeof waiting, wchan) == nullptr) waiting[0] = '\0';
        std::fclose(wchan);
    }
    char text[128];
    std::snprintf(text, sizeof text, "%c cpu=%llu%s%s", state, utime + stime,
                  waiting[0] != '\0' ? " in=" : "", waiting);
    return text;
}

// Every thread of this process with its name, scheduler state and wait channel, whether or not it
// ever ran guest code. Engine threads carry telling names (Flutter uses "1.ui", "1.raster",
// "1.io"), so this says which part of the guest is missing or waiting, and shows ART's own threads
// next to ours.
// Threads of the runtime itself (binder, ART daemons) crowd out the engine's own, and one report
// line holds only so much, so they are counted rather than listed and the rest goes out in chunks.
bool is_platform_thread(const char* name) {
    static const char* const kNoise[] = {"binder:", "Jit thread pool", "Profile Saver", "ADB-JDWP",
                                         "Signal Catcher", "perfetto", "HeapTaskDaemon",
                                         "ReferenceQueueD", "FinalizerDaemon", "FinalizerWatchd",
                                         "hwuiTask", "RenderThread", "queued-work-loo", "Timer-",
                                         "GPU completion", "SurfaceSyncGrou"};
    for (const char* prefix : kNoise) {
        if (std::strncmp(name, prefix, std::strlen(prefix)) == 0) return true;
    }
    return false;
}

void write_threads_census() {
    DIR* dir = ::opendir("/proc/self/task");
    if (dir == nullptr) {
        runtime_report().note_watch_detail("threads", "(unavailable)");
        return;
    }
    std::string chunk;
    unsigned chunk_index = 0;
    unsigned platform = 0;
    unsigned listed = 0;
    while (const dirent* entry = ::readdir(dir)) {
        if (entry->d_name[0] == '.') continue;
        const std::int32_t tid = std::atoi(entry->d_name);
        if (tid == 0) continue;
        char path[64];
        std::snprintf(path, sizeof path, "/proc/self/task/%d/comm", tid);
        char name[64] = {};
        if (std::FILE* comm = std::fopen(path, "re")) {
            if (std::fgets(name, sizeof name, comm) != nullptr) {
                char* newline = std::strchr(name, '\n');
                if (newline != nullptr) *newline = '\0';
            }
            std::fclose(comm);
        }
        if (is_platform_thread(name)) {
            ++platform;
            continue;
        }
        if (listed >= 48) break;
        ++listed;
        chunk += (chunk.empty() ? "" : " | ") + std::to_string(tid) + ":" + name + " " +
                 thread_kernel_state(tid);
        if (chunk.size() > 420) {
            runtime_report().note_watch_detail("threads-" + std::to_string(++chunk_index), chunk);
            chunk.clear();
        }
    }
    ::closedir(dir);
    if (!chunk.empty()) runtime_report().note_watch_detail("threads-" + std::to_string(++chunk_index), chunk);
    runtime_report().note_watch_detail("threads-platform", std::to_string(platform) + " runtime threads hidden");
}

std::string describe_thread_activity(const ThreadActivitySample& sample) {
    char text[64];
    switch (sample.kind) {
    case ThreadActivityKind::kSyscall:
        std::snprintf(text, sizeof text, "sys:%s%s", syscall_name(sample.id),
                      sample.in_progress ? "(inside)" : "");
        return text;
    case ThreadActivityKind::kHostCall:
        std::snprintf(text, sizeof text, "host:0x%x%s", sample.id, sample.in_progress ? "(inside)" : "");
        return text;
    case ThreadActivityKind::kNone:
    default:
        return "(none)";
    }
}

HangWatchdog::HangWatchdog(SnapshotFn snapshot) : snapshot_(std::move(snapshot)) {}

const char* thread_state_name(ThreadState state) {
    switch (state) {
    case ThreadState::kProgressing: return "progressing";
    case ThreadState::kIdleService: return "idle-service";
    case ThreadState::kIdleWait: return "idle-wait";
    case ThreadState::kLostWakeSuspect: return "lost-wake-suspect";
    case ThreadState::kSpinning: return "spinning";
    case ThreadState::kStalled: return "stalled";
    }
    return "?";
}

ThreadState classify_thread(const ThreadActivitySample& sample, bool changed, std::uint64_t cpu_delta,
                            bool waiting, bool word_changed, bool wake_missed) {
    // The library runtime's own parking: READY (0xFE00) on zbhost, PARK (0xFE01) on carriers.
    if (sample.kind == ThreadActivityKind::kHostCall && (sample.id == 0xFE00 || sample.id == 0xFE01)) {
        return ThreadState::kIdleService;
    }
    if (waiting) {
        if (word_changed || wake_missed) return ThreadState::kLostWakeSuspect;
        if (!changed) return ThreadState::kIdleWait;
    }
    if (changed) return ThreadState::kProgressing;
    return cpu_delta > 0 && !sample.in_progress ? ThreadState::kSpinning : ThreadState::kStalled;
}

namespace {

// CPU ticks of a task from /proc/self/task/<tid>/stat; 0 when unreadable.
std::uint64_t thread_cpu_ticks(std::int32_t tid) {
    char path[64];
    std::snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
    std::FILE* file = std::fopen(path, "re");
    if (file == nullptr) return 0;
    char line[1024];
    const char* read = std::fgets(line, sizeof line, file);
    std::fclose(file);
    if (read == nullptr) return 0;
    const char* cursor = std::strrchr(line, ')');
    unsigned long long utime = 0, stime = 0;
    if (cursor == nullptr ||
        std::sscanf(cursor + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &utime, &stime) != 2) {
        return 0;
    }
    return utime + stime;
}

std::string where(std::uint32_t addr) {
    if (addr == 0) return "?";
    const std::string d = futex_trace_describe_address(addr);
    if (!d.empty()) return d;
    char text[16];
    std::snprintf(text, sizeof text, "0x%x", addr);
    return text;
}

bool is_idle(ThreadState state) {
    return state == ThreadState::kIdleService || state == ThreadState::kIdleWait;
}

}  // namespace

bool HangWatchdog::sample(Clock::time_point now) {
    const std::vector<ThreadActivitySample> current = snapshot_();

    std::vector<State> next;
    next.reserve(current.size());
    for (const ThreadActivitySample& sample : current) {
        State* prior = nullptr;
        for (State& state : previous_) {
            if (state.tid == sample.tid) {
                prior = &state;
                break;
            }
        }
        State state;
        state.tid = sample.tid;
        state.kind = sample.kind;
        state.id = sample.id;
        state.counter = sample.counter;
        state.cpu = thread_cpu_ticks(sample.tid);
        const bool changed = prior == nullptr || prior->kind != sample.kind || prior->id != sample.id ||
                             prior->counter != sample.counter;
        if (!changed) {
            state.since = prior->since;
            state.unchanged_samples = prior->unchanged_samples + 1;
        } else {
            state.since = now;
            state.unchanged_samples = 0;
        }
        state.calls = prior != nullptr && sample.counter >= prior->counter ? sample.counter - prior->counter : 0;
        if (prior != nullptr) {
            state.pcs = prior->pcs;
            state.m68k_pcs = prior->m68k_pcs;
        }
        if (sample.pc != 0 && (state.pcs.empty() || state.pcs.back() != sample.pc)) {
            state.pcs.push_back(sample.pc);
            if (state.pcs.size() > 4) state.pcs.erase(state.pcs.begin());
        }
        const std::uint64_t cpu_delta = prior != nullptr && state.cpu > prior->cpu ? state.cpu - prior->cpu : 0;

        FutexWaitInfo wait;
        const bool waiting = sample.in_progress && futex_trace_current_wait(sample.tid, wait);
        bool wake_missed = false;
        if (waiting) {
            for (const FutexWakeInfo& wake : futex_trace_wakes_since(sample.tid, wait.uaddr, 8)) {
                if (wake.result == 0) wake_missed = true;
            }
        }
        state.state = classify_thread(sample, changed && prior != nullptr, cpu_delta, waiting,
                                      waiting && wait.current != wait.expected, wake_missed);

        // A rolling line per thread, overwritten every sample, so the report read at any time shows
        // what each guest thread is doing now and where in the guest it last left translated code.
        std::string live = std::string(thread_state_name(state.state)) + " calls+" + std::to_string(state.calls) +
                           " cpu+" + std::to_string(cpu_delta) + " last=" + describe_thread_activity(sample) +
                           " pc=" + where(sample.pc) + " lr=" + where(sample.lr);
        if (state.pcs.size() > 1) {
            live += " recent-pc=[";
            for (std::size_t i = 0; i < state.pcs.size(); ++i) live += (i ? ", " : "") + where(state.pcs[i]);
            live += "]";
        }
        // StyleTap: where the emulated 68K program is, from the last capture of this thread.
        {
            std::uint32_t words[kCaptureWords];
            std::uint32_t count = 0;
            if (latest_thread_capture(sample.tid, words, count)) {
                if (const std::optional<M68kState> m = find_styletap_m68k(words, count)) {
                    if (state.m68k_pcs.empty() || state.m68k_pcs.back() != m->pc) {
                        state.m68k_pcs.push_back(m->pc);
                        if (state.m68k_pcs.size() > 6) state.m68k_pcs.erase(state.m68k_pcs.begin());
                    }
                    char text[160];
                    std::snprintf(text, sizeof text, " m68k: pc=0x%x sr=0x%04x trap=0x%04x d0=0x%x a7=0x%x",
                                  m->pc, m->sr, m->trap, m->d0, m->a7);
                    live += text;
                    if (state.m68k_pcs.size() > 1) {
                        live += " recent-68k-pc=[";
                        for (std::size_t i = 0; i < state.m68k_pcs.size(); ++i) {
                            std::snprintf(text, sizeof text, "%s0x%x", i ? ", " : "", state.m68k_pcs[i]);
                            live += text;
                        }
                        live += "]";
                    }
                }
            }
            // Ask for a fresh one from threads that keep calling out; idle ones have nothing new.
            if (state.calls > 0) request_thread_capture(sample.tid);
        }
        if (waiting) {
            live += " wait=0x" + [&] {
                char text[16];
                std::snprintf(text, sizeof text, "%x", wait.uaddr);
                return std::string(text);
            }() + " waited=" + std::to_string(wait.waited_ms / 1000) + "s";
        }
        if (live_threads_ < kMaxLiveThreads || live_tids_.count(sample.tid)) {
            if (live_tids_.insert(sample.tid).second) ++live_threads_;
            runtime_report().note_watch_detail("live-" + std::to_string(sample.tid), live);
        }
        next.push_back(state);
    }
    previous_ = std::move(next);

    if (notes_written_ >= kMaxNotes) return false;

    // Only states that can be a hang raise notes; idle threads would otherwise use up the note
    // budget in the first seconds of every run, long before the hang being investigated.
    std::vector<const State*> stuck;
    for (const State& state : previous_) {
        if (state.unchanged_samples >= kStuckThreshold && !is_idle(state.state)) stuck.push_back(&state);
    }
    if (stuck.empty()) return false;

    std::string value;
    std::size_t shown = 0;
    for (const State* state : stuck) {
        if (shown >= kMaxThreadsShown) break;
        if (shown > 0) value += " | ";
        ThreadActivitySample sample;
        sample.tid = state->tid;
        sample.kind = state->kind;
        sample.id = state->id;
        sample.counter = state->counter;
        const auto seconds =
            std::chrono::duration_cast<std::chrono::seconds>(now - state->since).count();
        value += std::to_string(state->tid) + "=" + describe_thread_activity(sample) +
                 " x" + std::to_string(state->counter) + " stuck=" + std::to_string(seconds) +
                 " " + thread_state_name(state->state) + " " + thread_kernel_state(state->tid);
        ++shown;
    }
    value += " | recent-jni: " + jni_recent_calls();
    for (const State* state : stuck) {
        if (state->state != ThreadState::kLostWakeSuspect) continue;
        const std::string futex = futex_trace_describe(state->tid);
        if (!futex.empty()) runtime_report().note_watch_detail("futex-" + std::to_string(state->tid), futex);
    }
    // Once, with the first report: the whole thread census is long and does not change much.
    static bool census_written = false;
    if (!census_written) {
        census_written = true;
        write_threads_census();
    }

    ++notes_written_;
    runtime_report().note_watch_detail(std::to_string(notes_written_), value);
    return true;
}

void HangWatchdog::run_forever() {
    for (;;) {
        std::this_thread::sleep_for(kInterval);
        sample(Clock::now());
    }
}

void start_hang_watchdog() {
    static std::atomic<bool> started{false};
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true, std::memory_order_relaxed)) return;
    std::thread([]() {
        HangWatchdog watchdog;
        watchdog.run_forever();
    }).detach();
}

}  // namespace zb
