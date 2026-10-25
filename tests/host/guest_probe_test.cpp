// find_styletap_m68k: recognising StyleTap's 68K CPU object among captured words.
#include "zb/guest_probe.h"

#include <map>
#include <optional>

#include "check.h"

namespace {

using zb::styletap::kA7;
using zb::styletap::kD0;
using zb::styletap::kM68kVtable;
using zb::styletap::kPc;

constexpr std::uint32_t kBase = 0x40000000;   // where libStyleTap.so is loaded
constexpr std::uint32_t kObject = 0x10002000; // the M68K object

std::map<std::uint32_t, std::uint32_t> memory;

std::optional<std::uint32_t> read(std::uint32_t addr) {
    auto it = memory.find(addr);
    if (it == memory.end()) return std::nullopt;
    return it->second;
}

void build_object() {
    memory.clear();
    memory[kObject] = kBase + kM68kVtable + 8;
    memory[kObject + kD0] = 0x1234;
    memory[kObject + kA7] = 0x00017ffc;
    memory[kObject + kPc] = 0x0001a2b4;
    memory[kObject + 0xfc] = 0xdead4e4f;  // the opcode is the low half at 0xfc (TRAP #15)
    memory[kObject + 0x154] = 0xa0f70000; // trap is the high half: 0x156
    memory[0x10003000] = 0x99;            // a readable word that is not an object
}

void test_found_in_stack_words() {
    build_object();
    const std::uint32_t words[] = {0, 3, 0x10003000, 0xffff0000, kObject, 7};
    const auto m = zb::find_styletap_m68k(words, 6, kBase, read);
    CHECK(m.has_value());
    CHECK(m->object == kObject);
    CHECK(m->pc == 0x0001a2b4);
    CHECK(m->opcode == 0x4e4f);
    CHECK(m->trap == 0xa0f7);
    CHECK(m->d0 == 0x1234);
    CHECK(m->a7 == 0x00017ffc);
}

void test_wrong_base_finds_nothing() {
    build_object();
    const std::uint32_t words[] = {kObject};
    CHECK(!zb::find_styletap_m68k(words, 1, kBase + 0x1000, read).has_value());
}

void test_unreadable_fields_are_skipped() {
    build_object();
    memory.erase(kObject + kPc);
    const std::uint32_t words[] = {kObject};
    CHECK(!zb::find_styletap_m68k(words, 1, kBase, read).has_value());
}

void test_unaligned_and_null_are_skipped() {
    build_object();
    const std::uint32_t words[] = {0, kObject + 1};
    CHECK(!zb::find_styletap_m68k(words, 2, kBase, read).has_value());
}

void test_no_inspector_means_no_result() {
    const std::uint32_t words[] = {kObject};
    CHECK(!zb::find_styletap_m68k(words, 1).has_value());
}

}  // namespace

int main() {
    test_found_in_stack_words();
    test_wrong_base_finds_nothing();
    test_unreadable_fields_are_skipped();
    test_unaligned_and_null_are_skipped();
    test_no_inspector_means_no_result();
    std::puts("guest_probe_test: OK");
    return 0;
}
