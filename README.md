# ZettaBridge — StyleTap on Android 17

[한국어](README.ko.md) · **English** · [Original ZettaBridge README](docs/README-original.md)

> **Experimental development branch. StyleTap does not launch successfully yet.** This is a compatibility port, not an official StyleTap release.

## Goal

Run the legacy **StyleTap 0.8.0034** Android app (32-bit `armeabi-v7a` native library) on **ARM64-only Android 17** through ZettaBridge's 32-bit guest runtime. No root is intended to be required.

- Repository: `muns5965/ZettaBridge`
- Development branch: `feature/styletap-android17`
- Base project: ZettaBridge (see archived original documentation)
- **Do not merge experimental changes into `main` until tested.**

## Verified progress

| Step | Status |
| --- | --- |
| Debug launcher APK builds and installs | Verified |
| Import StyleTap APK | Verified |
| ARM32 guest linker starts | Verified after applying the Dynarmic patches |
| `libStyleTap.so` loads | Verified (`proxy-loads: 1`, `proxy-failures: 0`) |
| JNI native methods register | Verified (18 registered) |
| StyleTap Activity and UI start | **Not verified — crash** |
| Palm OS display, input and app functionality | **Not verified** |

### Current blocker

On Android 17, the last confirmed crash was:

```text
java.lang.NullPointerException:
  DummyEditText.addTextChangedListener(...) on a null object reference
at com.StyleTap.StyleTap.StyleTap.onCreate(StyleTap.java:2994)
```

The suspected cause is that the plugin Activity's window retained a host-context layout inflater. Commit `f074136b` rebinds the window inflater to the plugin Activity, **but this fix has not yet been verified on a device**. The earlier guest-linker `SIGILL` is no longer present in subsequent runtime reports.

## Build and test

1. Open [StyleTap launcher debug APK Actions](https://github.com/muns5965/ZettaBridge/actions/workflows/styletap-debug-apk.yml).
2. Select a successful run **on `feature/styletap-android17`** whose commit includes the latest changes; download the debug APK artifact.
3. Install ZettaBridge, import your **legally obtained** StyleTap APK, and launch it.
4. After a crash, reopen ZettaBridge, long-press StyleTap and select **Last run report**. The report includes guest runtime diagnostics, Java crash traces and available Android process exit information. Copy it for debugging; **ADB is not required**.

Debug CI builds may be signed with different keys; updating an installed APK can fail. Avoid uninstalling before protecting any locally stored data.

## Implementation notes

- The required Dynarmic compatibility patches live in `third_party/patches/`; CI must apply them rather than silently skip them.
- An experimental Android Bitmap host bridge is present; actual rendering is not yet verified.
- Launcher source changes trigger the development-branch debug APK workflow.
- Keep original/proprietary StyleTap APKs and native libraries **out of this repository**.

## Documentation

The prior general-purpose ZettaBridge documentation is preserved unmodified in:
- [Original English README](docs/README-original.md)
- [Original Russian README](docs/README-original.ru.md)

This branch's README intentionally describes **only the current StyleTap port**.
