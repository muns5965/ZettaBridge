#include "bitmap_runtime.h"
#include "zb/host_jni.h"
#include "zb/library_runtime.h"
#include "zb/platform_compat_hostcalls.h"
#include "zb/guest_memory.h"
#include <android/bitmap.h>
#include <jni.h>
#include <algorithm>
#include <cstring>
#include <limits>
namespace zb {
namespace {
void write32(unsigned char* p, std::uint32_t n) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<unsigned char>(n >> (8 * i));
}
}
bool BitmapRuntime::handle(std::uint32_t index, GuestThread& thread) {
    if (index != ZB_COMPAT_HC_AndroidBitmap_getInfo &&
        index != ZB_COMPAT_HC_AndroidBitmap_lockPixels &&
        index != ZB_COMPAT_HC_AndroidBitmap_unlockPixels) return false;
    auto& r = thread.regs();
    // Snapshot AAPCS32 arguments: nested guest malloc/free can overwrite registers.
    const std::uint32_t bitmap_handle = r[1];
    const std::uint32_t output_address = r[2];
    auto fail = [&] {
        r[0] = static_cast<std::uint32_t>(ANDROID_BITMAP_RESULT_BAD_PARAMETER);
        r[1] = 0;
        return true;
    };
    // Current JNI environment is only valid on the calling host thread.
    JNIEnv* env = reinterpret_cast<JNIEnv*>(
        static_cast<std::uintptr_t>(jni_.current_env()));
    if (busy_.test_and_set(std::memory_order_acquire)) return fail();
    struct ReleaseGate {
        std::atomic_flag& flag;
        ~ReleaseGate() { flag.clear(std::memory_order_release); }
    } release_gate{busy_};
    if (!env || !bitmap_handle) return fail();
    jobject bitmap = reinterpret_cast<jobject>(static_cast<std::uintptr_t>(
        jni_.resolve_ref(bitmap_handle, "AndroidBitmap")));
    if (!bitmap) return fail();
    AndroidBitmapInfo info{};
    if (index == ZB_COMPAT_HC_AndroidBitmap_getInfo) {
        if (!output_address ||
            AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS)
            return fail();
        unsigned char out[20]{};
        write32(out, info.width);
        write32(out + 4, info.height);
        write32(out + 8, info.stride);
        write32(out + 12, info.format);
        write32(out + 16, info.flags);
        auto* dest = runtime_.memory().host_ptr(output_address, sizeof(out), kPageWrite);
        if (!dest) return fail();
        std::memcpy(dest, out, sizeof(out));
        r[0] = ANDROID_BITMAP_RESULT_SUCCESS;
        r[1] = 0;
        return true;
    }
    auto equal = [&](const Active& a) {
        return env->IsSameObject(reinterpret_cast<jobject>(
            static_cast<std::uintptr_t>(a.global_ref)), bitmap) == JNI_TRUE;
    };
    auto it = std::find_if(active_.begin(), active_.end(), equal);
    if (index == ZB_COMPAT_HC_AndroidBitmap_unlockPixels) {
        if (it == active_.end() || it->guest_address == 0) return fail();
        auto* src = runtime_.memory().host_ptr(it->guest_address, it->bytes, kPageRead);
        if (!src) return fail();
        if (it->host_pixels != nullptr) std::memcpy(it->host_pixels, src, it->bytes);
        const int result = AndroidBitmap_unlockPixels(env,
            reinterpret_cast<jobject>(static_cast<std::uintptr_t>(it->global_ref)));
        // A failed unlock may keep the lock alive; do not dereference its pixel pointer again.
        if (result != ANDROID_BITMAP_RESULT_SUCCESS) {
            it->host_pixels = nullptr;
            return fail();
        }
        jobject owner = reinterpret_cast<jobject>(
            static_cast<std::uintptr_t>(it->global_ref));
        const std::uint32_t released_address = it->guest_address;
        active_.erase(it);
        GuestCall free_call{};
        free_call.regs = {released_address, 0, 0, 0};
        (void)runtime_.call_on_current(runtime_.service_api().free_fn, free_call);
        env->DeleteGlobalRef(owner);
        r[0] = ANDROID_BITMAP_RESULT_SUCCESS;
        r[1] = 0;
        return true;
    }
    if (!output_address || it != active_.end()) return fail();
    if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS)
        return fail();
    const std::uint32_t bpp = info.format == ANDROID_BITMAP_FORMAT_RGBA_8888 ? 4u :
        (info.format == ANDROID_BITMAP_FORMAT_RGB_565 ? 2u : 0u);
    const std::uint64_t count = static_cast<std::uint64_t>(info.stride) * info.height;
    if (!bpp || !info.height || !info.width ||
        static_cast<std::uint64_t>(info.width) * bpp > info.stride ||
        count == 0 || count > std::numeric_limits<std::int32_t>::max()) return fail();
    if (!runtime_.memory().host_ptr(output_address, 4, kPageWrite)) return fail();
    void* pixels = nullptr;
    if (AndroidBitmap_lockPixels(env, bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS)
        return fail();
    if (!pixels) {
        (void)AndroidBitmap_unlockPixels(env, bitmap);
        return fail();
    }
    jobject keep = env->NewGlobalRef(bitmap);
    if (!keep) {
        (void)AndroidBitmap_unlockPixels(env, bitmap);
        return fail();
    }
    const auto global_id = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(keep));
    active_.push_back({global_id, 0, 0, nullptr});
    auto cleanup = [&](std::uint32_t address) {
        if (address) {
            GuestCall free_call{};
            free_call.regs = {address, 0, 0, 0};
            (void)runtime_.call_on_current(runtime_.service_api().free_fn, free_call);
        }
        active_.erase(std::remove_if(active_.begin(), active_.end(),
            [&](const Active& a) { return a.global_ref == global_id; }), active_.end());
        env->DeleteGlobalRef(keep);
        (void)AndroidBitmap_unlockPixels(env, bitmap);
    };
    GuestCall alloc{};
    alloc.regs = {static_cast<std::uint32_t>(count), 0, 0, 0};
    auto res = runtime_.call_on_current(runtime_.service_api().malloc_fn, alloc);
    std::uint32_t address = res ? res->r0 : 0;
    auto* guest = address ?
        runtime_.memory().host_ptr(address, count, kPageWrite) : nullptr;
    if (!guest) {
        cleanup(address);
        return fail();
    }
    std::memcpy(guest, pixels, count);
    unsigned char addr[4];
    write32(addr, address);
    // Revalidate after guest malloc: it can affect guest memory mappings.
    auto* output = runtime_.memory().host_ptr(output_address, 4, kPageWrite);
    if (!output) {
        cleanup(address);
        return fail();
    }
    std::memcpy(output, addr, 4);
    auto pending = std::find_if(active_.begin(), active_.end(),
        [&](const Active& a) { return a.global_ref == global_id; });
    if (pending != active_.end()) {
        pending->guest_address = address;
        pending->bytes = static_cast<std::uint32_t>(count);
        pending->host_pixels = pixels;
    }
    r[0] = ANDROID_BITMAP_RESULT_SUCCESS;
    r[1] = 0;
    return true;
}
}  // namespace zb
