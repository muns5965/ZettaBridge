#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace zb {

// Futex diagnostics for the hang watchdog. Answers, for a guest thread found asleep in
// FUTEX_WAIT: which word it waits on, with which op/flags and expected value, from where in the
// guest it was called, whether it re-waited after our own EINTR, and who issued FUTEX_WAKE on
// that word afterwards and how many waiters each wake reported woken.
//
// Recording is lock-free and allocation-free: a fixed table of per-thread wait records and a
// fixed ring of wake records. Nothing here changes what the futex calls do.

constexpr std::size_t kFutexStackWords = 48;  // guest stack words copied at WAIT entry

struct FutexWaitInfo {
    std::int32_t tid = 0;          // guest tid (the host task id for ordinary guest threads)
    std::int32_t host_tid = 0;     // host task actually sleeping (differs on borrowed carriers)
    std::uint32_t uaddr = 0;       // guest address of the futex word
    std::uint32_t op = 0;          // as the guest passed it, FUTEX_PRIVATE_FLAG included
    std::uint32_t expected = 0;    // FUTEX_WAIT val
    std::uint32_t current = 0;     // the word's value when the report is written
    std::uint32_t pc = 0, lr = 0, sp = 0;
    std::uint64_t waited_ms = 0;
    std::uint32_t rewaits = 0;     // host EINTR with nothing to deliver, waited again
    std::uint32_t stack[kFutexStackWords] = {};
    std::uint32_t stack_words = 0;
};

struct FutexWakeInfo {
    std::uint64_t ms_after_wait = 0;  // when, relative to the waiter's WAIT entry
    std::int32_t tid = 0;
    std::int32_t host_tid = 0;
    std::uint32_t op = 0;
    std::uint32_t requested = 0;
    std::int32_t result = 0;          // waiters woken, or -errno
};

// Guest side, from sys_futex. `word` is the host pointer to the futex word, read at report time.
// `stack` points at up to kFutexStackWords readable guest stack words (may be null).
void futex_trace_wait_begin(std::int32_t tid, std::uint32_t uaddr, const volatile std::uint32_t* word,
                            std::uint32_t op, std::uint32_t expected, std::uint32_t pc, std::uint32_t lr,
                            std::uint32_t sp, const std::uint32_t* stack, std::uint32_t stack_words);
void futex_trace_wait_rewait(std::int32_t tid);
void futex_trace_wait_end(std::int32_t tid);
void futex_trace_wake(std::int32_t tid, std::uint32_t uaddr, std::uint32_t op, std::uint32_t requested,
                      std::int32_t result);

// Watchdog side. The thread's current wait, if it is inside one.
bool futex_trace_current_wait(std::int32_t tid, FutexWaitInfo& out);
// Wakes recorded on `uaddr` since that wait began, oldest first, at most `limit`.
std::vector<FutexWakeInfo> futex_trace_wakes_since(std::int32_t waiter_tid, std::uint32_t uaddr,
                                                   std::size_t limit);

// One report line for a waiting thread. `describe` turns a guest address into
// "libname 0x<offset>" or "" when it is not inside a file mapping (or the map is busy).
std::string futex_trace_describe(std::int32_t tid,
                                 const std::function<std::string(std::uint32_t)>& describe);

// The address describer the watchdog uses, installed once by the syscall layer. It must not block
// (the process map is read with try_lock) and returns "" when it cannot answer.
void futex_trace_set_describer(std::function<std::string(std::uint32_t)> describe);
// futex_trace_describe with that describer; "" when the thread is not waiting.
std::string futex_trace_describe(std::int32_t tid);

// Tests only.
void futex_trace_reset();

}  // namespace zb
