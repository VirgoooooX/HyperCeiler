/*
 * This file is part of HyperCeiler.
 *
 * HyperCeiler is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License.
 */
package com.sevtinge.hyperceiler.libhook.utils.os4;

import androidx.annotation.Keep;

/**
 * Build-specific native patches for the Flutter/Dart based HyperOS 4 launcher.
 *
 * The native side refuses to touch unknown libapp.so builds. This keeps an OTA
 * from turning a stale RVA into an arbitrary code patch.
 */
@Keep
public final class Os4LauncherNativeBridge {
    private static final String LIB_NAME = "hyperceiler_os4_launcher";

    private static boolean loadAttempted;
    private static boolean loaded;

    private Os4LauncherNativeBridge() {}

    private static synchronized boolean ensureLoaded() {
        if (loadAttempted) return loaded;
        loadAttempted = true;
        try {
            System.loadLibrary(LIB_NAME);
            loaded = true;
        } catch (Throwable ignored) {
            loaded = false;
        }
        return loaded;
    }

    public static boolean patchHotseat(int maxCount) {
        return ensureLoaded() && nativePatchHotseat(maxCount);
    }

    public static boolean patchGrid(int cellX, int cellY) {
        return ensureLoaded() && nativePatchGrid(cellX, cellY);
    }

    public static boolean patchIconSize(int iconSize) {
        return ensureLoaded() && nativePatchIconSize(iconSize);
    }

    private static native boolean nativePatchHotseat(int maxCount);
    private static native boolean nativePatchGrid(int cellX, int cellY);
    private static native boolean nativePatchIconSize(int iconSize);
}
