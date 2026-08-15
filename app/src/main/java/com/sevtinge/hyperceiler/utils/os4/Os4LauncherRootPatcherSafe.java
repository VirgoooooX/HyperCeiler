package com.sevtinge.hyperceiler.utils.os4;

import android.app.Application;
import android.content.Context;
import android.content.SharedPreferences;
import android.util.Log;

import com.sevtinge.hyperceiler.common.utils.PrefsBridge;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;

/**
 * Root-side HyperOS 4 launcher patcher for Xiaomi's hyos_spawner based
 * Flutter/Rust launcher. The shell watcher only tracks PIDs; all remote
 * process memory access is performed by the ARM64 native helper through
 * pread/pwrite on /proc/<pid>/mem.
 */
public final class Os4LauncherRootPatcherSafe {
    private static final String TAG = "HyperCeilerOS4Root";

    private static final String DAEMON_PATH = "/data/local/tmp/hyperceiler_os4_patcher.sh";
    private static final String PID_PATH = "/data/local/tmp/hyperceiler_os4_patcher.pid";
    private static final String LOG_PATH = "/data/local/tmp/hyperceiler_os4_patcher.log";

    private static final String KEY_HOTSEAT = "home_dock_unlock_hotseat";
    private static final String KEY_GRID = "home_layout_unlock_grids_new";
    private static final String KEY_CELL_X = "home_layout_unlock_grids_cell_x";
    private static final String KEY_CELL_Y = "home_layout_unlock_grids_cell_y";
    private static final String KEY_ICON_ENABLE = "home_title_icon_size_enable";
    private static final String KEY_ICON_SIZE = "home_title_icon_size";

    private static final ScheduledExecutorService EXECUTOR =
        Executors.newSingleThreadScheduledExecutor(r -> {
            Thread thread = new Thread(r, "HyperCeiler-OS4-RootPatcher");
            thread.setDaemon(true);
            return thread;
        });

    private static final Object LOCK = new Object();
    private static boolean initialized;
    private static ScheduledFuture<?> pendingSync;
    private static SharedPreferences.OnSharedPreferenceChangeListener preferenceListener;

    private Os4LauncherRootPatcherSafe() {}

    public static void initialize() {
        synchronized (LOCK) {
            if (initialized) return;
            initialized = true;

            SharedPreferences prefs = PrefsBridge.getSharedPreferences();
            if (prefs != null) {
                preferenceListener = (sharedPreferences, key) -> {
                    if (isRelevantKey(key)) scheduleSync(650L);
                };
                prefs.registerOnSharedPreferenceChangeListener(preferenceListener);
            }
        }
        scheduleSync(0L);
    }

    private static boolean isRelevantKey(String key) {
        if (key == null) return false;
        return key.equals(wrap(KEY_HOTSEAT))
            || key.equals(wrap(KEY_GRID))
            || key.equals(wrap(KEY_CELL_X))
            || key.equals(wrap(KEY_CELL_Y))
            || key.equals(wrap(KEY_ICON_ENABLE))
            || key.equals(wrap(KEY_ICON_SIZE));
    }

    private static String wrap(String key) {
        return key.startsWith("prefs_key_") ? key : "prefs_key_" + key;
    }

    private static void scheduleSync(long delayMs) {
        synchronized (LOCK) {
            if (pendingSync != null) pendingSync.cancel(false);
            pendingSync = EXECUTOR.schedule(
                Os4LauncherRootPatcherSafe::syncNow,
                delayMs,
                TimeUnit.MILLISECONDS
            );
        }
    }

    private static Context currentApplicationContext() {
        try {
            Class<?> activityThread = Class.forName("android.app.ActivityThread");
            Object app = activityThread.getMethod("currentApplication").invoke(null);
            if (app instanceof Application) return ((Application) app).getApplicationContext();
        } catch (Throwable t) {
            Log.e(TAG, "cannot resolve application context", t);
        }
        return null;
    }

    private static void syncNow() {
        Context context = currentApplicationContext();
        if (context == null) return;

        boolean hotseat = PrefsBridge.getBoolean(KEY_HOTSEAT);
        boolean grid = PrefsBridge.getBoolean(KEY_GRID);
        boolean icon = PrefsBridge.getBoolean(KEY_ICON_ENABLE);
        int cellX = clamp(PrefsBridge.getInt(KEY_CELL_X, 4), 3, 9);
        int cellY = clamp(PrefsBridge.getInt(KEY_CELL_Y, 6), 4, 13);
        int iconSize = clamp(PrefsBridge.getInt(KEY_ICON_SIZE, 182), 50, 360);

        String apkPath = context.getPackageCodePath();
        String nativeLibraryDir = context.getApplicationInfo().nativeLibraryDir;
        String helperLibrary = nativeLibraryDir + "/libhyperceiler_os4_root.so";

        String daemon = buildDaemonScript(
            apkPath,
            helperLibrary,
            hotseat,
            grid,
            icon,
            cellX,
            cellY,
            iconSize
        );
        String controller = buildControllerScript(
            daemon,
            hotseat || grid || icon,
            grid,
            cellX,
            cellY,
            helperLibrary
        );

        String output = runAsRoot(controller, 15_000L);
        if (output == null) {
            Log.e(TAG, "root controller failed or timed out");
            return;
        }
        Log.i(
            TAG,
            "native root patcher synced: hotseat=" + hotseat
                + " grid=" + grid + "(" + cellX + "x" + cellY + ")"
                + " icon=" + icon + "(" + iconSize + ")"
                + " helper=" + helperLibrary
                + " output=" + output.trim()
        );
    }

    private static String buildControllerScript(
        String daemon,
        boolean anyEnabled,
        boolean gridEnabled,
        int cellX,
        int cellY,
        String helperLibrary
    ) {
        StringBuilder sh = new StringBuilder(daemon.length() + 6144);
        sh.append("SCRIPT=").append(shellQuote(DAEMON_PATH)).append('\n');
        sh.append("PIDFILE=").append(shellQuote(PID_PATH)).append('\n');
        sh.append("LOGFILE=").append(shellQuote(LOG_PATH)).append('\n');

        // A failed/older controller can leave an orphan watcher whose PID is no
        // longer recorded in PIDFILE. Kill every old watcher by cmdline before
        // writing the new configuration so two generations cannot race and
        // continuously overwrite each other's in-memory return stubs.
        sh.append("for proc in /proc/[0-9]*; do\n");
        sh.append("  [ -r \"$proc/cmdline\" ] || continue\n");
        sh.append("  p=${proc#/proc/}\n");
        sh.append("  [ \"$p\" = \"$$\" ] && continue\n");
        sh.append("  cmd=$(tr '\\000' ' ' < \"$proc/cmdline\" 2>/dev/null)\n");
        sh.append("  case \"$cmd\" in *hyperceiler_os4_patcher.sh*) kill \"$p\" 2>/dev/null ;; esac\n");
        sh.append("done\n");
        sh.append("if [ -s \"$PIDFILE\" ]; then old=$(cat \"$PIDFILE\" 2>/dev/null); [ -n \"$old\" ] && kill \"$old\" 2>/dev/null; fi\n");
        sh.append("rm -f \"$PIDFILE\"\n");
        sh.append("sleep 0.12\n");

        if (gridEnabled) {
            sh.append("update_grid_file(){\n");
            sh.append("  file=\"$1\"; gx=\"$2\"; gy=\"$3\"; [ -f \"$file\" ] || return 1\n");
            sh.append("  tmp=/data/local/tmp/hyperceiler_grid_$$.xml\n");
            sh.append("  awk -v gx=\"$gx\" -v gy=\"$gy\" '\n");
            sh.append("    BEGIN { fx=0; fy=0 }\n");
            sh.append("    /<int[[:space:]]+name=\"pref_key_cell_x\"/ { sub(/value=\"[^\"]*\"/, \"value=\\\"\" gx \"\\\"\"); fx=1 }\n");
            sh.append("    /<int[[:space:]]+name=\"pref_key_cell_y\"/ { sub(/value=\"[^\"]*\"/, \"value=\\\"\" gy \"\\\"\"); fy=1 }\n");
            sh.append("    /<\\/map>/ { if (!fx) print \"    <int name=\\\"pref_key_cell_x\\\" value=\\\"\" gx \"\\\" />\"; if (!fy) print \"    <int name=\\\"pref_key_cell_y\\\" value=\\\"\" gy \"\\\" />\" }\n");
            sh.append("    { print }\n");
            sh.append("  ' \"$file\" > \"$tmp\" || { rm -f \"$tmp\"; return 1; }\n");
            sh.append("  cat \"$tmp\" > \"$file\" || { rm -f \"$tmp\"; return 1; }\n");
            sh.append("  rm -f \"$tmp\"; return 0\n");
            sh.append("}\n");
            sh.append("grid_updated=0\n");
            sh.append("for pref in /data/user/0/com.miui.home/shared_prefs/launcher_sharedpreference.xml /data/user_de/0/com.miui.home/shared_prefs/launcher_sharedpreference.xml; do\n");
            sh.append("  if update_grid_file \"$pref\" ").append(cellX).append(' ').append(cellY).append("; then echo \"grid prefs updated: $pref -> ").append(cellX).append('x').append(cellY).append("\"; grid_updated=1; fi\n");
            sh.append("done\n");
            sh.append("[ $grid_updated -eq 0 ] && echo 'WARNING launcher_sharedpreference.xml not found'\n");
        }

        if (!anyEnabled) {
            sh.append("pids=$(pidof com.miui.home 2>/dev/null); [ -n \"$pids\" ] && kill -9 $pids 2>/dev/null\n");
            sh.append("echo 'OS4 patcher disabled; launcher restarted'\n");
            return sh.toString();
        }

        sh.append("if [ ! -f ").append(shellQuote(helperLibrary)).append(" ]; then echo 'ERROR helper library missing: ").append(helperLibrary).append("'; exit 2; fi\n");
        sh.append("cat > \"$SCRIPT\" <<'HYPERCEILER_OS4_EOF'\n");
        sh.append(daemon);
        if (!daemon.endsWith("\n")) sh.append('\n');
        sh.append("HYPERCEILER_OS4_EOF\n");
        sh.append("chmod 600 \"$SCRIPT\"\n");
        sh.append("rm -f \"$LOGFILE\"\n");
        sh.append("if command -v setsid >/dev/null 2>&1; then setsid /system/bin/sh \"$SCRIPT\" >\"$LOGFILE\" 2>&1 < /dev/null & else /system/bin/sh \"$SCRIPT\" >\"$LOGFILE\" 2>&1 < /dev/null & fi\n");
        sh.append("i=0; while [ $i -lt 80 ] && [ ! -s \"$PIDFILE\" ]; do sleep 0.02; i=$((i+1)); done\n");
        sh.append("[ -s \"$PIDFILE\" ] || { echo 'daemon failed to start'; exit 3; }\n");
        sh.append("pids=$(pidof com.miui.home 2>/dev/null); [ -n \"$pids\" ] && kill -9 $pids 2>/dev/null\n");
        sh.append("echo 'daemon started pid='$(cat \"$PIDFILE\")\n");
        return sh.toString();
    }

    private static String buildDaemonScript(
        String apkPath,
        String helperLibrary,
        boolean hotseat,
        boolean grid,
        boolean icon,
        int cellX,
        int cellY,
        int iconSize
    ) {
        StringBuilder sh = new StringBuilder(4096);
        sh.append("#!/system/bin/sh\n");
        sh.append("PIDFILE=").append(shellQuote(PID_PATH)).append('\n');
        sh.append("APK=").append(shellQuote(apkPath)).append('\n');
        sh.append("LIB=").append(shellQuote(helperLibrary)).append('\n');
        sh.append("CLASS='com.sevtinge.hyperceiler.utils.os4.Os4LauncherRootPatcherCli'\n");
        sh.append("echo $$ > \"$PIDFILE\"\n");
        sh.append("trap 'rm -f \"$PIDFILE\"' EXIT INT TERM\n");
        sh.append("log(){ echo \"$(date '+%m-%d %H:%M:%S') $*\"; }\n");
        sh.append("log 'daemon(native) started: hotseat=").append(hotseat)
            .append(" grid=").append(grid).append(" cell=").append(cellX).append('x').append(cellY)
            .append(" icon=").append(icon).append(" iconSize=").append(iconSize).append("'\n");
        sh.append("log \"apk=$APK\"\n");
        sh.append("log \"helper=$LIB\"\n");
        sh.append("last_pid=''\n");
        sh.append("while :; do\n");
        sh.append("  pid=$(pidof com.miui.home 2>/dev/null | cut -d' ' -f1)\n");
        sh.append("  if [ -z \"$pid\" ]; then last_pid=''; sleep 0.02; continue; fi\n");
        sh.append("  if [ \"$pid\" = \"$last_pid\" ]; then sleep 0.20; continue; fi\n");
        sh.append("  log \"launcher candidate pid=$pid\"\n");
        sh.append("  CLASSPATH=\"$APK\" /system/bin/app_process /system/bin \"$CLASS\" \"$LIB\" \"$pid\" ")
            .append(hotseat).append(' ')
            .append(grid).append(' ')
            .append(cellX).append(' ')
            .append(cellY).append(' ')
            .append(icon).append(' ')
            .append(iconSize).append("\n");
        sh.append("  rc=$?; log \"native helper exit pid=$pid rc=$rc\"\n");
        sh.append("  if [ $rc -eq 10 ]; then sleep 0.08; continue; fi\n");
        sh.append("  last_pid=\"$pid\"\n");
        sh.append("  sleep 0.20\n");
        sh.append("done\n");
        return sh.toString();
    }

    private static String runAsRoot(String script, long timeoutMs) {
        Process process = null;
        try {
            process = new ProcessBuilder("su").redirectErrorStream(true).start();
            try (BufferedWriter writer = new BufferedWriter(
                new OutputStreamWriter(process.getOutputStream(), StandardCharsets.UTF_8)
            )) {
                writer.write(script);
                if (!script.endsWith("\n")) writer.newLine();
                writer.write("exit\n");
                writer.flush();
            }

            if (!process.waitFor(timeoutMs, TimeUnit.MILLISECONDS)) {
                process.destroyForcibly();
                return null;
            }

            StringBuilder output = new StringBuilder();
            try (BufferedReader reader = new BufferedReader(
                new InputStreamReader(process.getInputStream(), StandardCharsets.UTF_8)
            )) {
                String line;
                while ((line = reader.readLine()) != null) output.append(line).append('\n');
            }
            return output.toString();
        } catch (Throwable t) {
            Log.e(TAG, "root execution failed", t);
            if (process != null) process.destroyForcibly();
            return null;
        }
    }

    private static String shellQuote(String value) {
        return "'" + value.replace("'", "'\\''") + "'";
    }

    private static int clamp(int value, int min, int max) {
        return Math.max(min, Math.min(max, value));
    }
}
