package com.sevtinge.hyperceiler.utils.os4;

/**
 * Compatibility shim kept so existing app-side wiring does not need to know
 * which HyperOS 4 launcher transport is active.
 *
 * The old shell/per-PID patcher was intentionally retired after runtime logs
 * showed that byte-level Dart patches succeeded but did not affect the visible
 * launcher grid. Launcher 8.x establishes its authoritative DeviceParam in the
 * Rust libapp_launcher.so layer first, so all work now delegates to the early
 * resident watcher.
 */
public final class Os4LauncherRootPatcherSafe {
    private Os4LauncherRootPatcherSafe() {}

    public static void initialize() {
        Os4LauncherEarlyWatcher.initialize();
    }
}
