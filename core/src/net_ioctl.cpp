#include "zb/net_ioctl.h"

#include <net/if.h>

#include <algorithm>
#include <cstddef>
#include <cerrno>
#include <cstring>
#include <vector>

namespace zb {

static_assert(IFNAMSIZ == 16, "ifreq name");
static_assert(sizeof(struct sockaddr) == 16, "ifreq address");
static_assert(offsetof(struct ifreq, ifr_addr) == IFNAMSIZ, "ifreq layout");

IfconfResult translate_ifconf(std::int32_t guest_len, std::uint8_t* guest_buf,
                              const std::function<int(struct ifconf*)>& host_ioctl) {
    IfconfResult out;
    if (guest_len < 0) {
        out.rc = -EINVAL;
        return out;
    }
    struct ifconf host {};

    if (guest_buf == nullptr) {
        // Size query: the kernel reports the bytes it would write in host entries.
        host.ifc_len = 0;
        host.ifc_buf = nullptr;
        out.rc = host_ioctl(&host);
        if (out.rc != 0) return out;
        const std::uint32_t entries =
            std::min<std::uint32_t>(static_cast<std::uint32_t>(std::max(host.ifc_len, 0)) / sizeof(struct ifreq),
                                    kMaxIfconfEntries);
        out.entries = entries;
        out.guest_len = static_cast<std::int32_t>(entries * kGuestIfreqSize);
        return out;
    }

    const std::uint32_t capacity =
        std::min<std::uint32_t>(static_cast<std::uint32_t>(guest_len) / kGuestIfreqSize, kMaxIfconfEntries);
    std::vector<struct ifreq> entries(std::max<std::uint32_t>(capacity, 1));
    host.ifc_len = static_cast<int>(capacity * sizeof(struct ifreq));
    host.ifc_req = entries.data();
    out.rc = host_ioctl(&host);
    if (out.rc != 0) return out;

    const std::uint32_t got = std::min<std::uint32_t>(
        static_cast<std::uint32_t>(std::max(host.ifc_len, 0)) / sizeof(struct ifreq), capacity);
    for (std::uint32_t i = 0; i < got; ++i) {
        std::uint8_t* slot = guest_buf + i * kGuestIfreqSize;
        std::memcpy(slot, entries[i].ifr_name, IFNAMSIZ);
        std::memcpy(slot + IFNAMSIZ, &entries[i].ifr_addr, sizeof(struct sockaddr));
    }
    out.entries = got;
    out.guest_len = static_cast<std::int32_t>(got * kGuestIfreqSize);
    return out;
}

}  // namespace zb
