// Read-only guest inspection for diagnostics; see guest_probe.h.
#include "zb/guest_probe.h"

#include <atomic>
#include <mutex>

namespace zb {

namespace {
std::once_flag g_once;
GuestInspector g_inspector;
std::atomic<bool> g_set{false};
}  // namespace

void set_guest_inspector(GuestInspector inspector) {
    std::call_once(g_once, [&] {
        g_inspector = std::move(inspector);
        g_set.store(true, std::memory_order_release);
    });
}

const GuestInspector* guest_inspector() {
    return g_set.load(std::memory_order_acquire) ? &g_inspector : nullptr;
}

std::optional<M68kState> find_styletap_m68k(const std::uint32_t* words, std::size_t count, std::uint32_t lib_base,
                                            const std::function<std::optional<std::uint32_t>(std::uint32_t)>& read_u32) {
    using namespace styletap;
    const std::uint32_t vtable = lib_base + kM68kVtable + 8;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t object = words[i];
        if (object == 0 || (object & 3) != 0) continue;
        const std::optional<std::uint32_t> first = read_u32(object);
        if (!first || *first != vtable) continue;
        const auto pc = read_u32(object + kPc);
        const auto sr = read_u32(object + (kSr & ~3u));
        const auto trap = read_u32(object + (kTrap & ~3u));
        const auto d0 = read_u32(object + kD0);
        const auto a7 = read_u32(object + kA7);
        if (!pc || !sr || !trap || !d0 || !a7) continue;
        M68kState state;
        state.object = object;
        state.pc = *pc;
        // Little-endian guest: a 16-bit field at an offset that is 2 mod 4 is the high half.
        state.sr = static_cast<std::uint16_t>((kSr & 2) ? *sr >> 16 : *sr);
        state.trap = static_cast<std::uint16_t>((kTrap & 2) ? *trap >> 16 : *trap);
        state.d0 = *d0;
        state.a7 = *a7;
        return state;
    }
    return std::nullopt;
}

std::optional<M68kState> find_styletap_m68k(const std::uint32_t* words, std::size_t count) {
    const GuestInspector* inspector = guest_inspector();
    if (inspector == nullptr || !inspector->library_base || !inspector->read_u32) return std::nullopt;
    const std::optional<std::uint32_t> base = inspector->library_base(styletap::kLibrary);
    if (!base) return std::nullopt;
    return find_styletap_m68k(words, count, *base, inspector->read_u32);
}

}  // namespace zb
