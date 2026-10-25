#pragma once
#include <atomic>
#include <cstdint>
#include <vector>
#include "zb/guest_thread.h"
namespace zb {
class LibraryRuntime;
class HostJni;
class BitmapRuntime {
public:
    BitmapRuntime(LibraryRuntime& runtime, HostJni& jni) : runtime_(runtime), jni_(jni) {}
    bool handle(std::uint32_t index, GuestThread& thread);
private:
    struct Active {
        std::uint64_t global_ref;
        std::uint32_t guest_address;
        std::uint32_t bytes;
        void* host_pixels;
    };
    LibraryRuntime& runtime_;
    HostJni& jni_;
    std::atomic_flag busy_ = ATOMIC_FLAG_INIT;
    std::vector<Active> active_;
};
}  // namespace zb
