/*
 * This file is part of HyperCeiler.
 *
 * HyperCeiler is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License.
 */
package com.sevtinge.hyperceiler.libhook.utils.os4;

import android.util.Log;

import androidx.annotation.Keep;

/**
 * Build-specific native hooks for the Flutter/Dart based HyperOS 4 launcher.
 *
 * The native side refuses to hook unknown libapp.so builds. This keeps an OTA
 * from turning a stale RVA into an arbitrary native target.
 */
@Keep
public final class Os4LauncherNativeBridge {
    private static final String LIB_NAME = "hyperceiler_os4_launcher";
    private static final String TAG = "HyperCeilerOS4";

    private static boolean loadAttempted;
    private static boolean loaded;

    private Os4LauncherNativeBridge() {}

    private static synchronized boolean ensureLoaded() {
        if (loadAttempted) return loaded;
        loadAttempted = true;
        try {
            System.loadLibrary(LIB_NAME);
            loaded = true;
            Log.i(TAG, "Loaded lib" + LIB_NAME + ".so into launcher process");
        } catch (Throwable throwable) {
            loaded = false;
            Log.e(TAG, "Failed to load lib" + LIB_NAME + ".so", throwable);
        }
        return loaded;
    }

    public static boolean patchHotseat(int maxCount) {
        if (!ensureLoaded()) return false;
        boolean result = nativePatchHotseat(maxCount);
        Log.i(TAG, "patchHotseat(" + maxCount + ") result=" + result + " status=" + statusHex());
        return result;
    }

    public static boolean patchGrid(int cellX, int cellY) {
        if (!ensureLoaded()) return false;
        boolean result = nativePatchGrid(cellX, cellY);
        Log.i(TAG, "patchGrid(" + cellX + "x" + cellY + ") result=" + result + " status=" + statusHex());
        return result;
    }

    public static boolean patchIconSize(int iconSize) {
        if (!ensureLoaded()) return false;
        boolean result = nativePatchIconSize(iconSize);
        Log.i(TAG, "patchIconSize(" + iconSize + ") result=" + result + " status=" + statusHex());
        return result;
    }

    public static int getStatus() {
        if (!ensureLoaded()) return -1;
        return nativeGetStatus();
    }

    public static String statusHex() {
        int status = getStatus();
        return status < 0 ? "load-failed" : String.format("0x%03X", status);
    }

    private static native boolean nativePatchHotseat(int maxCount);
    private static native boolean nativePatchGrid(int cellX, int cellY);
    private static native boolean nativePatchIconSize(int iconSize);
    private static native int nativeGetStatus();
}
