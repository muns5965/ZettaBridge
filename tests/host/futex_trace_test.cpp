// futex_trace: wait records, wake history and the report line the hang watchdog writes.
#include "zb/futex_trace.h"

#include <linux/futex.h>

#include <cstring>
#include <string>
#include <thread>

#include "check.h"

namespace {

constexpr std::uint32_t kPrivate = FUTEX_PRIVATE_FLAG;

std::string fake_describe(std::uint32_t addr) {
    if (addr >= 0x10000 && addr < 0x20000) return "libStyleTap.so offset 0x" + std::to_string(addr - 0x10000);
    if (addr >= 0x40000 && addr < 0x50000) return "libc.so offset 0x" + std::to_string(addr - 0x40000);
    return "";
}

void test_wait_is_reported_while_active() {
    zb::futex_trace_reset();
    volatile std::uint32_t word = 5;
    const std::uint32_t stack[4] = {0x1234, 0x10042, 0x99, 0x10100};
    zb::futex_trace_wait_begin(101, 0x8000, &word, FUTEX_WAIT | kPrivate, 5, 0x40010, 0x40020, 0x7000, stack, 4);
    zb::FutexWaitInfo info;
    CHECK(zb::futex_trace_current_wait(101, info));
    CHECK(info.uaddr == 0x8000);
    CHECK(info.expected == 5);
    CHECK(info.current == 5);
    CHECK(info.op == (FUTEX_WAIT | kPrivate));
    CHECK(info.stack_words == 4);
    word = 6;  // the word changed under the sleeper
    const std::string line = zb::futex_trace_describe(101, fake_describe);
    CHECK(line.find("addr=0x8000") != std::string::npos);
    CHECK(line.find("WAIT|PRIVATE") != std::string::npos);
    CHECK(line.find("expect=0x5") != std::string::npos);
    CHECK(line.find("now=0x6") != std::string::npos);
    CHECK(line.find("pc=libc.so") != std::string::npos);
    // Only stack words inside a mapping are listed, nearest first.
    CHECK(line.find("stack-scan=[libStyleTap.so offset 0x66, libStyleTap.so offset 0x256]") != std::string::npos);
    CHECK(line.find("wakes-since=0") != std::string::npos);

    zb::futex_trace_wait_end(101);
    CHECK(!zb::futex_trace_current_wait(101, info));
    CHECK(zb::futex_trace_describe(101, fake_describe).empty());
}

void test_wakes_on_the_same_word_are_listed() {
    zb::futex_trace_reset();
    volatile std::uint32_t word = 0;
    zb::futex_trace_wait_begin(201, 0x9000, &word, FUTEX_WAIT | kPrivate, 0, 0, 0, 0, nullptr, 0);
    zb::futex_trace_wake(202, 0x9000, FUTEX_WAKE | kPrivate, 1, 0);   // woke nobody: a lost wake
    zb::futex_trace_wake(203, 0xA000, FUTEX_WAKE | kPrivate, 1, 1);   // another word, not listed
    zb::futex_trace_wake(204, 0x9000, FUTEX_WAKE, 1, 0);              // flags differ from the waiter
    const std::string line = zb::futex_trace_describe(201, fake_describe);
    CHECK(line.find("wakes-since=2") != std::string::npos);
    CHECK(line.find("tid=202 WAKE|PRIVATE n=1 woke=0") != std::string::npos);
    CHECK(line.find("tid=204 WAKE n=1 woke=0") != std::string::npos);
    CHECK(line.find("tid=203") == std::string::npos);
    zb::futex_trace_wait_end(201);
}

void test_rewaits_are_counted() {
    zb::futex_trace_reset();
    volatile std::uint32_t word = 1;
    zb::futex_trace_wait_begin(301, 0xB000, &word, FUTEX_WAIT, 1, 0, 0, 0, nullptr, 0);
    zb::futex_trace_wait_rewait(301);
    zb::futex_trace_wait_rewait(301);
    zb::FutexWaitInfo info;
    CHECK(zb::futex_trace_current_wait(301, info));
    CHECK(info.rewaits == 2);
    zb::futex_trace_wait_end(301);
}

void test_other_threads_read_a_waiting_record() {
    zb::futex_trace_reset();
    volatile std::uint32_t word = 3;
    std::thread waiter([&] {
        zb::futex_trace_wait_begin(401, 0xC000, &word, FUTEX_WAIT | kPrivate, 3, 0, 0, 0, nullptr, 0);
    });
    waiter.join();
    zb::FutexWaitInfo info;
    CHECK(zb::futex_trace_current_wait(401, info));
    CHECK(info.uaddr == 0xC000);
    CHECK(info.host_tid != 0);
}

void test_unknown_thread_has_no_wait() {
    zb::futex_trace_reset();
    zb::FutexWaitInfo info;
    CHECK(!zb::futex_trace_current_wait(999, info));
    CHECK(zb::futex_trace_describe(999).empty());
}

}  // namespace

int main() {
    test_wait_is_reported_while_active();
    test_wakes_on_the_same_word_are_listed();
    test_rewaits_are_counted();
    test_other_threads_read_a_waiting_record();
    test_unknown_thread_has_no_wait();
    std::puts("futex_trace_test: OK");
    return 0;
}
