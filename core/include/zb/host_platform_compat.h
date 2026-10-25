#pragma once

#include <cstdint>

#include "zb/guest_thread.h"

namespace zb {
class BitmapRuntime;

// Safe loader-compatibility fallbacks for platform entry points whose full pointer bridges are
// deferred. Every handled call is reported as unimplemented; pointer-returning or output-writing
// APIs return an explicit failure instead of letting the generic r0=0 path claim success.
class HostPlatformCompat {
public:
    void set_bitmap_runtime(BitmapRuntime* bitmap) { bitmap_ = bitmap; }
    bool handle_host_call(std::uint32_t index, GuestThread& thread);
private:
    BitmapRuntime* bitmap_ = nullptr;  // owned by process-lifetime guest engine
};

}  // namespace zb
