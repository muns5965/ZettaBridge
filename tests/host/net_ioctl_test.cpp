// SIOCGIFCONF conversion between the arm32 guest layout (32-byte ifreq) and the host's.
#include "zb/net_ioctl.h"

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

#include "check.h"

namespace {

struct FakeIf {
    const char* name;
    const char* ipv4;
};

const FakeIf kIfs[] = {{"lo", "127.0.0.1"}, {"wlan0", "192.168.0.23"}, {"rmnet_data0", "10.11.12.13"}};
constexpr int kCount = 3;

// Behaves like the kernel: a null buffer reports the needed length, otherwise whole entries
// that fit are written and ifc_len becomes the bytes written.
int fake_kernel(struct ifconf* ifc) {
    if (ifc->ifc_buf == nullptr) {
        ifc->ifc_len = kCount * static_cast<int>(sizeof(struct ifreq));
        return 0;
    }
    const int fit = ifc->ifc_len / static_cast<int>(sizeof(struct ifreq));
    const int n = fit < kCount ? fit : kCount;
    for (int i = 0; i < n; ++i) {
        struct ifreq* r = &ifc->ifc_req[i];
        std::memset(r, 0xAB, sizeof *r);  // garbage in host-only bytes must not leak
        std::memset(r->ifr_name, 0, IFNAMSIZ);
        std::strncpy(r->ifr_name, kIfs[i].name, IFNAMSIZ - 1);
        sockaddr_in sin{};
        sin.sin_family = AF_INET;
        inet_pton(AF_INET, kIfs[i].ipv4, &sin.sin_addr);
        std::memcpy(&r->ifr_addr, &sin, sizeof sin);
    }
    ifc->ifc_len = n * static_cast<int>(sizeof(struct ifreq));
    return 0;
}

void check_entry(const std::uint8_t* slot, int i) {
    CHECK(std::strcmp(reinterpret_cast<const char*>(slot), kIfs[i].name) == 0);
    sockaddr_in sin;
    std::memcpy(&sin, slot + 16, sizeof sin);
    CHECK(sin.sin_family == AF_INET);
    char text[INET_ADDRSTRLEN];
    CHECK(inet_ntop(AF_INET, &sin.sin_addr, text, sizeof text) != nullptr);
    CHECK(std::strcmp(text, kIfs[i].ipv4) == 0);
}

void test_size_query() {
    const zb::IfconfResult r = zb::translate_ifconf(0, nullptr, fake_kernel);
    CHECK(r.rc == 0);
    CHECK(r.entries == kCount);
    CHECK(r.guest_len == kCount * 32);  // guest entries, not the host's 40-byte ones
}

void test_full_buffer() {
    std::vector<std::uint8_t> guest(kCount * 32 + 8, 0xEE);
    const zb::IfconfResult r = zb::translate_ifconf(kCount * 32, guest.data(), fake_kernel);
    CHECK(r.rc == 0);
    CHECK(r.entries == kCount);
    CHECK(r.guest_len == kCount * 32);
    for (int i = 0; i < kCount; ++i) check_entry(guest.data() + i * 32, i);
    for (std::size_t i = kCount * 32; i < guest.size(); ++i) CHECK(guest[i] == 0xEE);
}

void test_small_buffer() {
    // Room for one entry plus a partial one: only whole entries, never past ifc_len.
    std::vector<std::uint8_t> guest(64, 0xEE);
    const zb::IfconfResult r = zb::translate_ifconf(32 + 20, guest.data(), fake_kernel);
    CHECK(r.rc == 0);
    CHECK(r.entries == 1);
    CHECK(r.guest_len == 32);
    check_entry(guest.data(), 0);
    for (std::size_t i = 32; i < guest.size(); ++i) CHECK(guest[i] == 0xEE);
}

void test_zero_length_buffer() {
    std::uint8_t guest[4] = {0xEE, 0xEE, 0xEE, 0xEE};
    const zb::IfconfResult r = zb::translate_ifconf(0, guest, fake_kernel);
    CHECK(r.rc == 0);
    CHECK(r.entries == 0);
    CHECK(r.guest_len == 0);
    CHECK(guest[0] == 0xEE);
}

void test_negative_length() {
    std::uint8_t guest[32];
    bool called = false;
    const zb::IfconfResult r = zb::translate_ifconf(-1, guest, [&](struct ifconf*) {
        called = true;
        return 0;
    });
    CHECK(r.rc == -EINVAL);
    CHECK(!called);
}

void test_kernel_error_passes_through() {
    std::uint8_t guest[64];
    const zb::IfconfResult r = zb::translate_ifconf(64, guest, [](struct ifconf*) { return -EBADF; });
    CHECK(r.rc == -EBADF);
}

void test_real_host_kernel() {
    // The host kernel accepts the converted request and returns at least loopback.
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;  // no sockets in this sandbox
    auto real = [fd](struct ifconf* ifc) { return ::ioctl(fd, SIOCGIFCONF, ifc) == 0 ? 0 : -errno; };
    const zb::IfconfResult size = zb::translate_ifconf(0, nullptr, real);
    CHECK(size.rc == 0);
    std::vector<std::uint8_t> guest(size.guest_len > 0 ? size.guest_len : 32);
    const zb::IfconfResult list = zb::translate_ifconf(static_cast<std::int32_t>(guest.size()), guest.data(), real);
    CHECK(list.rc == 0);
    CHECK(list.guest_len % 32 == 0);
    CHECK(list.entries == static_cast<std::uint32_t>(list.guest_len / 32));
    ::close(fd);
}

}  // namespace

int main() {
    test_size_query();
    test_full_buffer();
    test_small_buffer();
    test_zero_length_buffer();
    test_negative_length();
    test_kernel_error_passes_through();
    test_real_host_kernel();
    std::puts("net_ioctl_test: OK");
    return 0;
}
