package com.sevtinge.hyperceiler.common.log

import android.util.Log
import com.sevtinge.hyperceiler.common.utils.PrefsBridge
import io.github.libxposed.api.XposedInterface

/**
 * Xposed 日志工具类
 */
object XposedLog {
    private const val TAG = "HyperCeiler"
    private const val DUAL_ROW_LOGCAT_TAG = "DualRowSignal"
    private const val SYSTEMUI_PACKAGE = "com.android.systemui"

    @Volatile
    private var sXposed: XposedInterface? = null

    @Volatile
    private var sDualRowGateLogged = false

    @JvmStatic
    fun init(xposed: XposedInterface) {
        sXposed = xposed
        sDualRowGateLogged = false
    }

    /**
     * Emit the actual dual-row gate values from the SystemUI hook process.
     *
     * This intentionally runs before the normal Xposed log-level filter: the
     * diagnostic must remain visible even when HyperCeiler logging is set to
     * error-only. Reaching this method also proves that the SystemUI BaseLoad
     * path itself is alive; a completely absent line therefore points above
     * DualRowSignalHookV/SystemUIB's phone-specific gate.
     */
    private fun probeDualRowGate(pkg: String?) {
        if (pkg != SYSTEMUI_PACKAGE || sDualRowGateLogged) return
        synchronized(this) {
            if (sDualRowGateLogged) return
            sDualRowGateLogged = true

            try {
                val enabled = PrefsBridge.getBoolean("system_ui_statusbar_network_icon_enable")
                val signalMode = PrefsBridge.getStringAsInt(
                    "system_ui_status_bar_icon_mobile_network_signal_mode",
                    0
                )
                val hideCard1 = PrefsBridge.getBoolean(
                    "system_ui_status_bar_icon_mobile_network_hide_card_1"
                )
                val hideCard2 = PrefsBridge.getBoolean(
                    "system_ui_status_bar_icon_mobile_network_hide_card_2"
                )
                val shouldInit = enabled && signalMode == 0 && !hideCard1 && !hideCard2

                Log.i(
                    DUAL_ROW_LOGCAT_TAG,
                    "SystemUI gate reached: hookProcess=${PrefsBridge.isHookProcess()} " +
                        "dualRowPref=$enabled signalMode=$signalMode " +
                        "hideCard1=$hideCard1 hideCard2=$hideCard2 shouldInit=$shouldInit"
                )
            } catch (t: Throwable) {
                Log.e(
                    DUAL_ROW_LOGCAT_TAG,
                    "SystemUI gate reached but PrefsBridge read failed: ${t.javaClass.name}: ${t.message}",
                    t
                )
            }
        }
    }

    @Suppress("DEPRECATION")
    private fun logRaw(priority: Int, msg: String, t: Throwable? = null) {
        val xposed = sXposed
        if (xposed != null) {
            xposed.log(priority, TAG, msg, t)

            // Dual-row signal adaptation is currently being validated on
            // HyperOS 4 / API 37. Mirror only its diagnostics to Android's
            // logcat so runtime failures can be collected with adb without
            // exporting the complete LSPosed module log.
            if (msg.contains("DualRowSignal", ignoreCase = true)) {
                Log.println(priority, DUAL_ROW_LOGCAT_TAG, msg)
                t?.let {
                    Log.println(priority, DUAL_ROW_LOGCAT_TAG, Log.getStackTraceString(it))
                }
            }
        } else {
            Log.println(priority, TAG, msg)
            t?.let { Log.println(priority, TAG, Log.getStackTraceString(it)) }
        }
    }

    private fun priorityToLevel(priority: Int): String = when (priority) {
        Log.DEBUG -> "D"
        Log.INFO -> "I"
        Log.WARN -> "W"
        Log.ERROR -> "E"
        else -> "V"
    }

    private fun shouldLog(requiredLevel: Int): Boolean {
        return LoggerUtils.shouldLog(LogStatusManager.getLogLevel(), requiredLevel)
    }

    // --- Full logs: 2 ---
    @JvmStatic
    fun d(msg: String) {
        if (!shouldLog(LogLevelManager.LEVEL_VERBOSE)) return
        logRaw(Log.DEBUG, msg)
    }

    @JvmStatic
    fun d(tag: String, msg: String) {
        if (!shouldLog(LogLevelManager.LEVEL_VERBOSE)) return
        logRaw(Log.DEBUG, "[$tag]: $msg")
    }

    @JvmStatic
    fun d(tag: String, msg: String, t: Throwable) {
        if (!shouldLog(LogLevelManager.LEVEL_VERBOSE)) return
        logRaw(Log.DEBUG, "[$tag]: $msg", t)
    }

    @JvmStatic
    fun d(tag: String, pkg: String?, msg: String) {
        probeDualRowGate(pkg)
        if (!shouldLog(LogLevelManager.LEVEL_VERBOSE)) return
        logRaw(Log.DEBUG, LoggerUtils.formatBrackets(pkg, tag, msg))
    }

    // --- Full logs: 2 ---
    @JvmStatic
    fun i(msg: String) {
        if (!shouldLog(LogLevelManager.LEVEL_VERBOSE)) return
        logRaw(Log.INFO, msg)
    }

    @JvmStatic
    fun i(tag: String, msg: String) {
        if (!shouldLog(LogLevelManager.LEVEL_VERBOSE)) return
        logRaw(Log.INFO, "[$tag]: $msg")
    }

    @JvmStatic
    fun i(tag: String, pkg: String?, msg: String) {
        probeDualRowGate(pkg)
        if (!shouldLog(LogLevelManager.LEVEL_VERBOSE)) return
        logRaw(Log.INFO, LoggerUtils.formatBrackets(pkg, tag, msg))
    }

    // --- Full logs: 2 ---
    @JvmStatic
    fun w(msg: String) {
        if (!shouldLog(LogLevelManager.LEVEL_VERBOSE)) return
        logRaw(Log.WARN, msg)
    }

    @JvmStatic
    fun w(tag: String, msg: String) {
        if (!shouldLog(LogLevelManager.LEVEL_VERBOSE)) return
        logRaw(Log.WARN, "[$tag]: $msg")
    }

    @JvmStatic
    fun w(tag: String, msg: String, t: Throwable) {
        if (!shouldLog(LogLevelManager.LEVEL_VERBOSE)) return
        logRaw(Log.WARN, "[$tag]: $msg", t)
    }

    @JvmStatic
    fun w(tag: String, pkg: String?, msg: String) {
        probeDualRowGate(pkg)
        if (!shouldLog(LogLevelManager.LEVEL_VERBOSE)) return
        logRaw(Log.WARN, LoggerUtils.formatBrackets(pkg, tag, msg))
    }

    @JvmStatic
    fun w(tag: String, pkg: String?, msg: String, t: Throwable) {
        probeDualRowGate(pkg)
        if (!shouldLog(LogLevelManager.LEVEL_VERBOSE)) return
        logRaw(Log.WARN, LoggerUtils.formatBrackets(pkg, tag, msg), t)
    }

    // --- Error only: 1 ---
    @JvmStatic
    fun e(msg: String) {
        if (!shouldLog(LogLevelManager.LEVEL_ERROR_ONLY)) return
        logRaw(Log.ERROR, msg)
    }

    @JvmStatic
    fun e(tag: String, msg: String) {
        if (!shouldLog(LogLevelManager.LEVEL_ERROR_ONLY)) return
        logRaw(Log.ERROR, "[$tag]: $msg")
    }

    @JvmStatic
    fun e(tag: String, t: Throwable) {
        if (!shouldLog(LogLevelManager.LEVEL_ERROR_ONLY)) return
        logRaw(Log.ERROR, "[$tag]: ${t.message ?: t.toString()}", t)
    }

    @JvmStatic
    fun e(tag: String, msg: String, t: Throwable) {
        if (!shouldLog(LogLevelManager.LEVEL_ERROR_ONLY)) return
        logRaw(Log.ERROR, "[$tag]: $msg", t)
    }

    @JvmStatic
    fun e(tag: String, pkg: String?, msg: String) {
        probeDualRowGate(pkg)
        if (!shouldLog(LogLevelManager.LEVEL_ERROR_ONLY)) return
        logRaw(Log.ERROR, LoggerUtils.formatBrackets(pkg, tag, msg))
    }

    @JvmStatic
    fun e(tag: String, pkg: String?, msg: String, t: Throwable) {
        probeDualRowGate(pkg)
        if (!shouldLog(LogLevelManager.LEVEL_ERROR_ONLY)) return
        logRaw(Log.ERROR, LoggerUtils.formatBrackets(pkg, tag, msg), t)
    }

    @JvmStatic
    fun logLevelDesc(): String {
        return LoggerUtils.logLevelDesc(LogStatusManager.getLogLevel())
    }
}
