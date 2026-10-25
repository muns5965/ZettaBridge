#pragma once

#include <cstdint>
#include <functional>

struct ifconf;

namespace zb {

// SIOCGIFCONF between an arm32 guest and a 64-bit host.
//
// The guest's struct ifconf is { int32 ifc_len; uint32 ifc_buf; } and each struct ifreq is 32
// bytes: a 16-byte name and a 16-byte union whose largest member is a sockaddr. On a 64-bit
// host the union holds a struct ifmap with two longs, so struct ifreq is 40 bytes and
// struct ifconf carries an 8-byte pointer. The kernel only fills the name and the sockaddr for
// SIOCGIFCONF, so each entry converts by copying those 32 bytes and nothing else.
constexpr unsigned long kSiocGifconf = 0x8912;
constexpr std::uint32_t kGuestIfreqSize = 32;
// More interfaces than any phone has; bounds the host buffer for a hostile ifc_len.
constexpr std::uint32_t kMaxIfconfEntries = 256;

struct GuestIfconf {
    std::int32_t len;
    std::uint32_t buf;
};
static_assert(sizeof(GuestIfconf) == 8, "arm32 struct ifconf");

struct IfconfResult {
    std::int32_t rc = 0;          // 0 or -errno, the ioctl result for the guest
    std::int32_t guest_len = 0;   // bytes to store in the guest's ifc_len when rc == 0
    std::uint32_t entries = 0;    // interfaces reported to the guest
};

// Runs SIOCGIFCONF on the host through `host_ioctl` (0 or -errno) and converts the result.
// `guest_buf` is the guest buffer already translated to a host pointer, or null when the guest
// asked for the needed size (ifc_buf == 0). `guest_len` is the guest's ifc_len in bytes.
// Writes at most guest_len bytes, rounded down to whole 32-byte entries, into guest_buf.
IfconfResult translate_ifconf(std::int32_t guest_len, std::uint8_t* guest_buf,
                              const std::function<int(struct ifconf*)>& host_ioctl);

}  // namespace zb
