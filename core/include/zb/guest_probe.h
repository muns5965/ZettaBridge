#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace zb {

// Read-only guest inspection for diagnostics, installed once by the syscall layer. Both callbacks
// must not block (the process map is read with try_lock) and answer nullopt when they cannot.
struct GuestInspector {
    // Guest address where the library with this file name was loaded (its offset-0 segment).
    std::function<std::optional<std::uint32_t>(const std::string&)> library_base;
    // A 32-bit guest word, if the page is mapped and readable.
    std::function<std::optional<std::uint32_t>(std::uint32_t)> read_u32;
};

void set_guest_inspector(GuestInspector inspector);
// Null until the syscall layer installed one.
const GuestInspector* guest_inspector();

// StyleTap 0.8.0034's 68K CPU object (class M68K in libStyleTap.so), for telling a loop in the
// emulated Palm program from one in StyleTap's own ARM code. Layout from M68K::M68K and
// M68K::CopyToEmulationState: vtable pointer at +0, D0-D7 at +0x8c, A0-A7 at +0xac, PC at +0xcc,
// SR (16 bit) at +0xfc, current or last Palm OS trap number (16 bit) at +0x156. Inert for any
// other guest: the object is only recognised by StyleTap's vtable address.
namespace styletap {
constexpr const char* kLibrary = "libStyleTap.so";
constexpr std::uint32_t kM68kVtable = 0x192fc8;  // _ZTV4M68K; objects point 8 bytes in
constexpr std::uint32_t kD0 = 0x8c;
constexpr std::uint32_t kA7 = 0xac + 7 * 4;
constexpr std::uint32_t kPc = 0xcc;
constexpr std::uint32_t kSr = 0xfc;
constexpr std::uint32_t kTrap = 0x156;
}  // namespace styletap

struct M68kState {
    std::uint32_t object = 0;  // guest address of the M68K object
    std::uint32_t pc = 0;      // 68K program counter (a Palm address)
    std::uint16_t sr = 0;
    std::uint16_t trap = 0;    // Palm OS trap (0xA0xx..) running now or last
    std::uint32_t d0 = 0;
    std::uint32_t a7 = 0;
};

// Looks through `words` (callee-saved registers and stack words of a StyleTap thread) for a
// pointer to an M68K object and reads its state. `lib_base` is where libStyleTap.so is loaded.
std::optional<M68kState> find_styletap_m68k(const std::uint32_t* words, std::size_t count, std::uint32_t lib_base,
                                            const std::function<std::optional<std::uint32_t>(std::uint32_t)>& read_u32);

// The same, with the installed inspector; nullopt without one or when StyleTap is not loaded.
std::optional<M68kState> find_styletap_m68k(const std::uint32_t* words, std::size_t count);

}  // namespace zb
