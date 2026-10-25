// Futex diagnostics for hangs; see futex_trace.h. Lock-free, allocation-free recording.
#include "zb/futex_trace.h"

#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>

namespace zb {

namespace {

constexpr std::size_t kMaxWaitSlots = 256;
constexpr std::size_t kWakeRing = 512;

std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

std::int32_t host_tid() { return static_cast<std::int32_t>(::syscall(SYS_gettid)); }

// One per guest thread, claimed on its first wait. `seq` is odd while the owner writes, so a
// reader on another thread can tell a torn copy and skip it.
struct WaitSlot {
    std::atomic<std::int32_t> tid{0};
    std::atomic<std::uint32_t> seq{0};
    std::atomic<bool> active{false};
    std::int32_t host_tid = 0;
    std::uint32_t uaddr = 0, op = 0, expected = 0, pc = 0, lr = 0, sp = 0;
    const volatile std::uint32_t* word = nullptr;
    std::uint64_t begin_ms = 0;
    std::atomic<std::uint32_t> rewaits{0};
    std::uint32_t stack[kFutexStackWords] = {};
    std::uint32_t stack_words = 0;
};

struct WakeSlot {
    std::atomic<std::uint64_t> seq{0};  // 0 = never written; otherwise the global wake number + 1
    std::uint64_t at_ms = 0;
    std::int32_t tid = 0, host_tid = 0;
    std::uint32_t uaddr = 0, op = 0, requested = 0;
    std::int32_t result = 0;
};

WaitSlot g_waits[kMaxWaitSlots];
WakeSlot g_wakes[kWakeRing];
std::atomic<std::uint64_t> g_wake_count{0};

WaitSlot* slot_for(std::int32_t tid, bool claim) {
    static thread_local WaitSlot* mine = nullptr;
    static thread_local std::int32_t mine_tid = 0;
    if (claim && mine != nullptr && mine_tid == tid && mine->tid.load(std::memory_order_relaxed) == tid) return mine;
    WaitSlot* free_slot = nullptr;
    for (WaitSlot& slot : g_waits) {
        const std::int32_t current = slot.tid.load(std::memory_order_acquire);
        if (current == tid) {
            if (claim) {
                mine = &slot;
                mine_tid = tid;
            }
            return &slot;
        }
        if (current == 0 && free_slot == nullptr) free_slot = &slot;
    }
    if (!claim || free_slot == nullptr) return nullptr;
    std::int32_t expected = 0;
    if (!free_slot->tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel)) return nullptr;
    mine = free_slot;
    mine_tid = tid;
    return free_slot;
}

const char* op_name(std::uint32_t op) {
    switch (op & 0x7f) {
    case 0: return "WAIT";
    case 1: return "WAKE";
    case 9: return "WAIT_BITSET";
    case 10: return "WAKE_BITSET";
    default: return "OP";
    }
}

std::string op_text(std::uint32_t op) {
    std::string text = op_name(op);
    if (op & 128) text += "|PRIVATE";
    if (op & 256) text += "|REALTIME";
    return text;
}

std::string hex(std::uint32_t value) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%x", value);
    return buf;
}

}  // namespace

void futex_trace_wait_begin(std::int32_t tid, std::uint32_t uaddr, const volatile std::uint32_t* word,
                            std::uint32_t op, std::uint32_t expected, std::uint32_t pc, std::uint32_t lr,
                            std::uint32_t sp, const std::uint32_t* stack, std::uint32_t stack_words) {
    WaitSlot* slot = slot_for(tid, true);
    if (slot == nullptr) return;  // table full: diagnostics only
    slot->seq.fetch_add(1, std::memory_order_acq_rel);  // odd: writing
    slot->host_tid = host_tid();
    slot->uaddr = uaddr;
    slot->word = word;
    slot->op = op;
    slot->expected = expected;
    slot->pc = pc;
    slot->lr = lr;
    slot->sp = sp;
    slot->begin_ms = now_ms();
    slot->rewaits.store(0, std::memory_order_relaxed);
    slot->stack_words = 0;
    if (stack != nullptr) {
        const std::uint32_t n = stack_words < kFutexStackWords ? stack_words : kFutexStackWords;
        for (std::uint32_t i = 0; i < n; ++i) slot->stack[i] = stack[i];
        slot->stack_words = n;
    }
    slot->active.store(true, std::memory_order_relaxed);
    slot->seq.fetch_add(1, std::memory_order_acq_rel);  // even: stable
}

void futex_trace_wait_rewait(std::int32_t tid) {
    if (WaitSlot* slot = slot_for(tid, true)) slot->rewaits.fetch_add(1, std::memory_order_relaxed);
}

void futex_trace_wait_end(std::int32_t tid) {
    if (WaitSlot* slot = slot_for(tid, true)) slot->active.store(false, std::memory_order_release);
}

void futex_trace_wake(std::int32_t tid, std::uint32_t uaddr, std::uint32_t op, std::uint32_t requested,
                      std::int32_t result) {
    const std::uint64_t n = g_wake_count.fetch_add(1, std::memory_order_relaxed);
    WakeSlot& slot = g_wakes[n % kWakeRing];
    slot.seq.store(0, std::memory_order_release);  // invalid while rewritten
    slot.at_ms = now_ms();
    slot.tid = tid;
    slot.host_tid = host_tid();
    slot.uaddr = uaddr;
    slot.op = op;
    slot.requested = requested;
    slot.result = result;
    slot.seq.store(n + 1, std::memory_order_release);
}

bool futex_trace_current_wait(std::int32_t tid, FutexWaitInfo& out) {
    WaitSlot* slot = slot_for(tid, false);
    if (slot == nullptr) return false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const std::uint32_t before = slot->seq.load(std::memory_order_acquire);
        if (before & 1) continue;
        if (!slot->active.load(std::memory_order_acquire)) return false;
        FutexWaitInfo info;
        info.tid = tid;
        info.host_tid = slot->host_tid;
        info.uaddr = slot->uaddr;
        info.op = slot->op;
        info.expected = slot->expected;
        info.current = slot->word != nullptr ? *slot->word : 0;
        info.pc = slot->pc;
        info.lr = slot->lr;
        info.sp = slot->sp;
        const std::uint64_t now = now_ms();
        info.waited_ms = now > slot->begin_ms ? now - slot->begin_ms : 0;
        info.rewaits = slot->rewaits.load(std::memory_order_relaxed);
        info.stack_words = slot->stack_words;
        for (std::uint32_t i = 0; i < info.stack_words; ++i) info.stack[i] = slot->stack[i];
        if (slot->seq.load(std::memory_order_acquire) == before) {
            out = info;
            return true;
        }
    }
    return false;
}

std::vector<FutexWakeInfo> futex_trace_wakes_since(std::int32_t waiter_tid, std::uint32_t uaddr,
                                                   std::size_t limit) {
    std::vector<FutexWakeInfo> out;
    WaitSlot* waiter = slot_for(waiter_tid, false);
    if (waiter == nullptr) return out;
    const std::uint64_t since = waiter->begin_ms;
    const std::uint64_t total = g_wake_count.load(std::memory_order_relaxed);
    const std::uint64_t first = total > kWakeRing ? total - kWakeRing : 0;
    for (std::uint64_t n = first; n < total && out.size() < limit; ++n) {
        const WakeSlot& slot = g_wakes[n % kWakeRing];
        if (slot.seq.load(std::memory_order_acquire) != n + 1) continue;  // overwritten or in flight
        if (slot.uaddr != uaddr || slot.at_ms < since) continue;
        FutexWakeInfo info;
        info.ms_after_wait = slot.at_ms - since;
        info.tid = slot.tid;
        info.host_tid = slot.host_tid;
        info.op = slot.op;
        info.requested = slot.requested;
        info.result = slot.result;
        if (slot.seq.load(std::memory_order_acquire) == n + 1) out.push_back(info);
    }
    return out;
}

std::string futex_trace_describe(std::int32_t tid,
                                 const std::function<std::string(std::uint32_t)>& describe) {
    FutexWaitInfo w;
    if (!futex_trace_current_wait(tid, w)) return "";
    std::string text = "addr=" + hex(w.uaddr) + " " + op_text(w.op) + " expect=" + hex(w.expected) +
                       " now=" + hex(w.current) + " waited=" + std::to_string(w.waited_ms / 1000) + "s" +
                       " rewaits=" + std::to_string(w.rewaits) + " host=" + std::to_string(w.host_tid);
    auto where = [&](std::uint32_t addr) {
        const std::string d = describe(addr);
        return d.empty() ? hex(addr) : d;
    };
    text += " pc=" + where(w.pc) + " lr=" + where(w.lr);
    // Heuristic backtrace: stack words that fall inside a mapped library, nearest first. Data
    // pointers into a library's .data look the same, so treat this as a hint, not a backtrace.
    std::string frames;
    unsigned shown = 0;
    for (std::uint32_t i = 0; i < w.stack_words && shown < 6; ++i) {
        const std::string d = describe(w.stack[i]);
        if (d.empty()) continue;
        frames += (shown == 0 ? "" : ", ") + d;
        ++shown;
    }
    text += " stack-scan=[" + frames + "]";
    const std::vector<FutexWakeInfo> wakes = futex_trace_wakes_since(tid, w.uaddr, 4);
    text += " wakes-since=" + std::to_string(wakes.size());
    for (const FutexWakeInfo& k : wakes) {
        text += " [+" + std::to_string(k.ms_after_wait) + "ms tid=" + std::to_string(k.tid) + " " +
                op_text(k.op) + " n=" + std::to_string(k.requested) + " woke=" + std::to_string(k.result) + "]";
    }
    return text;
}

namespace {
std::once_flag g_describer_once;
std::function<std::string(std::uint32_t)> g_describer;
std::atomic<bool> g_describer_set{false};
}  // namespace

void futex_trace_set_describer(std::function<std::string(std::uint32_t)> describe) {
    std::call_once(g_describer_once, [&] {
        g_describer = std::move(describe);
        g_describer_set.store(true, std::memory_order_release);
    });
}

std::string futex_trace_describe_address(std::uint32_t addr) {
    return g_describer_set.load(std::memory_order_acquire) ? g_describer(addr) : std::string();
}

std::string futex_trace_describe(std::int32_t tid) {
    static const std::function<std::string(std::uint32_t)> none = [](std::uint32_t) { return std::string(); };
    return futex_trace_describe(tid, g_describer_set.load(std::memory_order_acquire) ? g_describer : none);
}

void futex_trace_reset() {
    for (WaitSlot& slot : g_waits) {
        slot.tid.store(0);
        slot.active.store(false);
        slot.seq.store(0);
    }
    for (WakeSlot& slot : g_wakes) slot.seq.store(0);
    g_wake_count.store(0);
}

}  // namespace zb
