/*
 * This file is part of HyperCeiler.
 *
 * HyperCeiler is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License.
 */
package com.sevtinge.hyperceiler.libhook.rules.home

import android.app.Application
import android.content.Context
import com.sevtinge.hyperceiler.common.log.XposedLog
import com.sevtinge.hyperceiler.common.utils.PrefsBridge
import com.sevtinge.hyperceiler.libhook.appbase.mihome.HomeBaseHookNew
import com.sevtinge.hyperceiler.libhook.appbase.mihome.Version
import com.sevtinge.hyperceiler.libhook.utils.api.ContextUtils
import com.sevtinge.hyperceiler.libhook.utils.os4.Os4LauncherNativeBridge
import io.github.lingqiqi5211.ezhooktool.xposed.common.HookParam
import io.github.lingqiqi5211.ezhooktool.xposed.java.IMethodHook

/**
 * Compatibility layer for the Flutter/Dart based HyperOS 4 system launcher.
 *
 * The 8.x launcher no longer exposes the Java DeviceConfigs/GridConfig methods
 * used by the HyperOS 3 hooks. Keep OS4 handling here so the old hooks remain
 * untouched for 6.x/7.x launcher builds.
 */
class Os4LauncherCompat : HomeBaseHookNew() {

    @Version(min = 800000000, max = 899999999)
    private fun initOS4Hook() {
        val unlockHotseat = PrefsBridge.getBoolean("home_dock_unlock_hotseat")
        val unlockGrid = PrefsBridge.getBoolean("home_layout_unlock_grids_new")
        val customizeIconSize = PrefsBridge.getBoolean("home_title_icon_size_enable")
        val cellX = PrefsBridge.getInt("home_layout_unlock_grids_cell_x", 4).coerceIn(3, 9)
        val cellY = PrefsBridge.getInt("home_layout_unlock_grids_cell_y", 6).coerceIn(4, 13)
        val iconSize = PrefsBridge.getInt("home_title_icon_size", 182).coerceIn(50, 360)

        XposedLog.i(
            TAG,
            lpparam.packageName,
            "OS4 entry: hotseat=$unlockHotseat grid=$unlockGrid iconSize=$customizeIconSize " +
                "cell=${cellX}x${cellY} iconPx=$iconSize"
        )

        if (unlockHotseat) {
            if (Os4LauncherNativeBridge.patchHotseat(99)) {
                XposedLog.i(
                    TAG,
                    lpparam.packageName,
                    "OS4: configured hotseat max-count; native=${Os4LauncherNativeBridge.statusHex()}"
                )
            } else {
                XposedLog.e(
                    TAG,
                    lpparam.packageName,
                    "OS4: failed hotseat native setup; native=${Os4LauncherNativeBridge.statusHex()}"
                )
            }
        }

        if (unlockGrid) {
            if (Os4LauncherNativeBridge.patchGrid(cellX, cellY)) {
                XposedLog.i(
                    TAG,
                    lpparam.packageName,
                    "OS4: configured grid ${cellX}x${cellY}; native=${Os4LauncherNativeBridge.statusHex()}"
                )
            } else {
                XposedLog.e(
                    TAG,
                    lpparam.packageName,
                    "OS4: failed grid native setup; native=${Os4LauncherNativeBridge.statusHex()}"
                )
            }
        }

        if (customizeIconSize) {
            if (Os4LauncherNativeBridge.patchIconSize(iconSize)) {
                XposedLog.i(
                    TAG,
                    lpparam.packageName,
                    "OS4: configured icon size $iconSize; native=${Os4LauncherNativeBridge.statusHex()}"
                )
            } else {
                XposedLog.e(
                    TAG,
                    lpparam.packageName,
                    "OS4: failed icon-size native setup; native=${Os4LauncherNativeBridge.statusHex()}"
                )
            }
        }

        // PreferenceUtils in the 8.01 launcher persists the grid in
        // launcher_sharedpreference using pref_key_cell_x/pref_key_cell_y.
        // Write it before Flutter initializes GridConfig, and restore the
        // original values after the HyperCeiler switch is turned off.
        findAndHookMethod(
            Application::class.java,
            "attach",
            Context::class.java,
            object : IMethodHook {
                override fun before(param: HookParam) {
                    val context = param.args[0] as? Context ?: return
                    applyGridPreferences(context, unlockGrid, cellX, cellY)
                    XposedLog.i(
                        TAG,
                        lpparam.packageName,
                        "OS4 Application.attach: grid prefs applied; native=${Os4LauncherNativeBridge.statusHex()}"
                    )
                }
            }
        )

        // Also cover module hot-reload where Application.attach already ran.
        ContextUtils.getContextNoError(ContextUtils.FLAG_CURRENT_APP)?.let {
            applyGridPreferences(it, unlockGrid, cellX, cellY)
            XposedLog.i(
                TAG,
                lpparam.packageName,
                "OS4 current app context available; native=${Os4LauncherNativeBridge.statusHex()}"
            )
        }

        if (PrefsBridge.getBoolean("home_layout_workspace_padding_bottom_enable") ||
            PrefsBridge.getBoolean("home_layout_workspace_padding_top_enable") ||
            PrefsBridge.getBoolean("home_layout_workspace_padding_horizontal_enable")
        ) {
            XposedLog.w(
                TAG,
                lpparam.packageName,
                "OS4: workspace padding still uses the legacy Java hook and is not adapted by this patch"
            )
        }
    }

    override fun initBase() {
        // HyperOS 3 and older continue to use their existing Java hooks.
    }

    private fun applyGridPreferences(context: Context, enabled: Boolean, cellX: Int, cellY: Int) {
        val prefs = context.getSharedPreferences(OS4_LAUNCHER_PREFS, Context.MODE_PRIVATE)
        if (enabled) {
            val editor = prefs.edit()
            if (!prefs.getBoolean(BACKUP_MARKER, false)) {
                val hadX = prefs.contains(KEY_CELL_X)
                val hadY = prefs.contains(KEY_CELL_Y)
                editor
                    .putBoolean(BACKUP_MARKER, true)
                    .putBoolean(BACKUP_HAD_X, hadX)
                    .putBoolean(BACKUP_HAD_Y, hadY)
                if (hadX) editor.putInt(BACKUP_CELL_X, prefs.getInt(KEY_CELL_X, 4))
                if (hadY) editor.putInt(BACKUP_CELL_Y, prefs.getInt(KEY_CELL_Y, 6))
            }
            val committed = editor
                .putInt(KEY_CELL_X, cellX)
                .putInt(KEY_CELL_Y, cellY)
                .commit()
            if (committed) {
                XposedLog.i(TAG, lpparam.packageName, "OS4: persisted launcher grid ${cellX}x${cellY}")
            } else {
                XposedLog.w(TAG, lpparam.packageName, "OS4: failed to persist custom launcher grid")
            }
            return
        }

        if (!prefs.getBoolean(BACKUP_MARKER, false)) return

        val editor = prefs.edit()
        if (prefs.getBoolean(BACKUP_HAD_X, false)) {
            editor.putInt(KEY_CELL_X, prefs.getInt(BACKUP_CELL_X, 4))
        } else {
            editor.remove(KEY_CELL_X)
        }
        if (prefs.getBoolean(BACKUP_HAD_Y, false)) {
            editor.putInt(KEY_CELL_Y, prefs.getInt(BACKUP_CELL_Y, 6))
        } else {
            editor.remove(KEY_CELL_Y)
        }
        val committed = editor
            .remove(BACKUP_MARKER)
            .remove(BACKUP_HAD_X)
            .remove(BACKUP_HAD_Y)
            .remove(BACKUP_CELL_X)
            .remove(BACKUP_CELL_Y)
            .commit()
        if (committed) {
            XposedLog.i(TAG, lpparam.packageName, "OS4: restored original launcher grid preferences")
        } else {
            XposedLog.w(TAG, lpparam.packageName, "OS4: failed to restore original launcher grid")
        }
    }

    companion object {
        private const val OS4_LAUNCHER_PREFS = "launcher_sharedpreference"
        private const val KEY_CELL_X = "pref_key_cell_x"
        private const val KEY_CELL_Y = "pref_key_cell_y"

        private const val BACKUP_MARKER = "hyperceiler_os4_grid_backup_v1"
        private const val BACKUP_HAD_X = "hyperceiler_os4_grid_backup_had_x"
        private const val BACKUP_HAD_Y = "hyperceiler_os4_grid_backup_had_y"
        private const val BACKUP_CELL_X = "hyperceiler_os4_grid_backup_x"
        private const val BACKUP_CELL_Y = "hyperceiler_os4_grid_backup_y"
    }
}
