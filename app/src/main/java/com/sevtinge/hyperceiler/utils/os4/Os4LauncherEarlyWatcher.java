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
 * HyperOS 4 launcher 8.x compatibility controller.
 *
 * Xiaomi starts this launcher through hyos_spawner and the authoritative grid
 * state is initialized in libapp_launcher.so (Rust) before Flutter consumes
 * it. This controller therefore starts one resident uid-0 native watcher,
 * waits until that watcher is actually ready, and only then kills the current
 * launcher so the replacement process can be patched during early startup.
 */
public final class Os4LauncherEarlyWatcher {
    private static final String TAG = "HyperCeilerOS4Early";

    private static final String SCRIPT_PATH = "/data/local/tmp/hyperceiler_os4_watcher.sh";
    private static final String PID_PATH = "/data/local/tmp/hyperceiler_os4_watcher.pid";
    private static final String READY_PATH = "/data/local/tmp/hyperceiler_os4_watcher.ready";
    // Keep the historical path so the user's existing adb command still works.
    private static final String LOG_PATH = "/data/local/tmp/hyperceiler_os4_patcher.log";

    private static final String KEY_HOTSEAT = "home_dock_unlock_hotseat";
    private static final String KEY_GRID = "home_layout_unlock_grids_new";
    private static final String KEY_CELL_X = "home_layout_unlock_grids_cell_x";
    private static final String KEY_CELL_Y = "home_layout_unlock_grids_cell_y";
    private static final String KEY_ICON_ENABLE = "home_title_icon_size_enable";
    private static final String KEY_ICON_SIZE = "home_title_icon_size";

    private static final ScheduledExecutorService EXECUTOR =
        Executors.newSingleThreadScheduledExecutor(r -> {
            Thread thread = new Thread(r, "HyperCeiler-OS4-EarlyWatcher");
            thread.setDaemon(true);
            return thread;
        });

    private static final Object LOCK = new Object();
    private static boolean initialized;
    private static ScheduledFuture<?> pendingSync;
    private static SharedPreferences.OnSharedPreferenceChangeListener preferenceListener;

    private Os4LauncherEarlyWatcher() {}

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
                Os4LauncherEarlyWatcher::syncNow,
                delayMs,
                TimeUnit.MILLISECONDS
            );
        }
    }

    private static Context currentApplicationContext() {
        try {
            Class<?> activityThread = Class.forName("android.app.ActivityThread");
            Object app = activityThread.getMethod("currentApplication").invoke(null);
            if (app instanceof Application) {
                return ((Application) app).getApplicationContext();
            }
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
        String helperLibrary = context.getApplicationInfo().nativeLibraryDir
            + "/libhyperceiler_os4_root.so";
        String controller = buildControllerScript(
            apkPath,
            helperLibrary,
            hotseat,
            grid,
            icon,
            cellX,
            cellY,
            iconSize
        );

        String output = runAsRoot(controller, 20_000L);
        if (output == null) {
            Log.e(TAG, "early watcher controller failed or timed out");
            return;
        }
        Log.i(
            TAG,
            "early watcher synced hotseat=" + hotseat
                + " grid=" + grid + "(" + cellX + "x" + cellY + ")"
                + " icon=" + icon + "(" + iconSize + ") output=" + output.trim()
        );
    }

    private static String buildControllerScript(
        String apkPath,
        String helperLibrary,
        boolean hotseat,
        boolean grid,
        boolean icon,
        int cellX,
        int cellY,
        int iconSize
    ) {
        boolean anyEnabled = hotseat || grid || icon;
        StringBuilder sh = new StringBuilder(12_000);
        sh.append("SCRIPT=").append(shellQuote(SCRIPT_PATH)).append('\n');
        sh.append("PIDFILE=").append(shellQuote(PID_PATH)).append('\n');
        sh.append("READY=").append(shellQuote(READY_PATH)).append('\n');
        sh.append("LOGFILE=").append(shellQuote(LOG_PATH)).append('\n');

        // Clean both old shell generations and resident app_process generations.
        sh.append("for proc in /proc/[0-9]*; do\n");
        sh.append("  [ -r \"$proc/cmdline\" ] || continue\n");
        sh.append("  p=${proc#/proc/}; [ \"$p\" = \"$$\" ] && continue\n");
        sh.append("  cmd=$(tr '\\000' ' ' < \"$proc/cmdline\" 2>/dev/null)\n");
        sh.append("  case \"$cmd\" in\n");
        sh.append("    *hyperceiler_os4_patcher.sh*|*hyperceiler_os4_watcher.sh*|*Os4LauncherRootPatcherCli*watch*) kill \"$p\" 2>/dev/null ;;\n");
        sh.append("  esac\n");
        sh.append("done\n");
        sh.append("for pf in /data/local/tmp/hyperceiler_os4_patcher.pid \"$PIDFILE\"; do\n");
        sh.append("  if [ -s \"$pf\" ]; then old=$(cat \"$pf\" 2>/dev/null); [ -n \"$old\" ] && kill \"$old\" 2>/dev/null; fi\n");
        sh.append("done\n");
        sh.append("rm -f /data/local/tmp/hyperceiler_os4_patcher.pid \"$PIDFILE\" \"$READY\"\n");
        sh.append("sleep 0.12\n");

        if (grid) {
            appendGridPreferenceUpdate(sh, cellX, cellY);
        }

        if (!anyEnabled) {
            sh.append("pids=$(pidof com.miui.home 2>/dev/null); [ -n \"$pids\" ] && kill -9 $pids 2>/dev/null\n");
            sh.append("echo 'OS4 early watcher disabled; launcher restarted'\n");
            return sh.toString();
        }

        sh.append("if [ ! -f ").append(shellQuote(helperLibrary)).append(" ]; then\n");
        sh.append("  echo 'ERROR helper library missing: ").append(helperLibrary).append("' > \"$LOGFILE\"\n");
        sh.append("  cat \"$LOGFILE\"; exit 2\n");
        sh.append("fi\n");

        sh.append("cat > \"$SCRIPT\" <<'HYPERCEILER_OS4_WATCHER_EOF'\n");
        sh.append("#!/system/bin/sh\n");
        sh.append("PIDFILE=").append(shellQuote(PID_PATH)).append('\n');
        sh.append("APK=").append(shellQuote(apkPath)).append('\n');
        sh.append("LIB=").append(shellQuote(helperLibrary)).append('\n');
        sh.append("CLASS='com.sevtinge.hyperceiler.utils.os4.Os4LauncherRootPatcherCli'\n");
        sh.append("echo $$ > \"$PIDFILE\"\n");
        sh.append("export CLASSPATH=\"$APK\"\n");
        sh.append("exec /system/bin/app_process /system/bin \"$CLASS\" watch \"$LIB\" ")
            .append(hotseat).append(' ')
            .append(grid).append(' ')
            .append(cellX).append(' ')
            .append(cellY).append(' ')
            .append(icon).append(' ')
            .append(iconSize).append('\n');
        sh.append("HYPERCEILER_OS4_WATCHER_EOF\n");
        sh.append("chmod 600 \"$SCRIPT\"\n");
        sh.append("rm -f \"$LOGFILE\" \"$READY\"\n");
        sh.append("if command -v setsid >/dev/null 2>&1; then\n");
        sh.append("  setsid /system/bin/sh \"$SCRIPT\" >\"$LOGFILE\" 2>&1 < /dev/null &\n");
        sh.append("else\n");
        sh.append("  /system/bin/sh \"$SCRIPT\" >\"$LOGFILE\" 2>&1 < /dev/null &\n");
        sh.append("fi\n");

        // Critical ordering: do not restart launcher until nativeWatch() itself is ready.
        sh.append("i=0\n");
        sh.append("while [ $i -lt 250 ] && [ ! -s \"$READY\" ]; do sleep 0.02; i=$((i+1)); done\n");
        sh.append("if [ ! -s \"$READY\" ]; then\n");
        sh.append("  echo 'ERROR resident native watcher did not become ready'\n");
        sh.append("  cat \"$LOGFILE\" 2>/dev/null\n");
        sh.append("  exit 3\n");
        sh.append("fi\n");
        sh.append("pids=$(pidof com.miui.home 2>/dev/null)\n");
        sh.append("[ -n \"$pids\" ] && kill -9 $pids 2>/dev/null\n");
        sh.append("echo 'resident watcher ready pid='$(cat \"$READY\")'; launcher restart requested'\n");
        return sh.toString();
    }

    private static void appendGridPreferenceUpdate(StringBuilder sh, int cellX, int cellY) {
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
        sh.append("  if update_grid_file \"$pref\" ").append(cellX).append(' ').append(cellY)
            .append("; then echo \"grid prefs updated: $pref -> ").append(cellX).append('x').append(cellY)
            .append("\"; grid_updated=1; fi\n");
        sh.append("done\n");
        sh.append("[ $grid_updated -eq 0 ] && echo 'WARNING launcher_sharedpreference.xml not found'\n");
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
