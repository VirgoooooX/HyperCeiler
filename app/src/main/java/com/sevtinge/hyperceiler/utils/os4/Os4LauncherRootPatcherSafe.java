package com.sevtinge.hyperceiler.utils.os4;

import android.content.SharedPreferences;
import android.util.Log;

import com.sevtinge.hyperceiler.common.utils.PrefsBridge;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;
import java.util.Locale;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;

/**
 * Safe root-side patch path for the HyperOS 4 Flutter/Rust launcher.
 *
 * The real launcher process is spawned by Xiaomi's hyos_spawner and is not a
 * normal Zygote/ART process. This helper therefore runs from the HyperCeiler
 * app, starts a root watcher, and patches only targets whose return ABI has
 * been verified from the supplied launcher binary.
 *
 * Grid handling deliberately does NOT patch DeviceConfig.cellCountX/Y. Those
 * getters return object-valued state and are not safe to replace with a Smi.
 * Instead, the launcher's own persisted pref_key_cell_x/pref_key_cell_y values
 * are updated before a clean launcher restart, while the verified numeric
 * min/max/default getters are patched in libapp.so.
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

    // RELEASE-8.01.02.5334-260807-08151151-R
    // libapp.so Build ID 4f1bdaed80328aa4b22817f1e00300bf.
    private static final String SUPPORTED_BUILD_NOTE_HEX =
        "040000001000000003000000474e5500" +
            "4f1bdaed80328aa4b22817f1e00300bf";

    // In the supplied APK the stored libapp.so entry starts at 0x1cfc000.
    // This is used only as a fast map lookup. The in-memory GNU Build ID is
    // always verified before any patch, and a Build-ID scan is kept as fallback.
    private static final long KNOWN_LIBAPP_APK_OFFSET = 0x01CFC000L;
    private static final long RVA_BUILD_NOTE = 0x000001C8L;
    private static final long RVA_HOTSEAT_MAX = 0x008F0500L;
    private static final long RVA_CELL_X_MAX = 0x0095CA90L;
    private static final long RVA_CELL_X_MIN = 0x00978264L;
    private static final long RVA_CELL_X_DEF = 0x009A7CF8L;
    private static final long RVA_ICON_SIZE = 0x009AA734L;
    private static final long RVA_CELL_Y_DEF = 0x00B57280L;

    private static final byte[] EXPECT_CONFIG_GETTER = hex("22f041b842801c8b");
    private static final byte[] EXPECT_HOTSEAT = hex("fd79bfa9fd030faa");
    private static final byte[] EXPECT_ICON = hex("fd79bfa9fd030faaef8100d1");

    private static final ScheduledExecutorService EXECUTOR =
        Executors.newSingleThreadScheduledExecutor(r -> {
            Thread thread = new Thread(r, "HyperCeiler-OS4-RootPatcher");
            thread.setDaemon(true);
            return thread;
        });

    private static final Object LOCK = new Object();
    private static boolean initialized;
    private static ScheduledFuture<?> pendingSync;
    // SharedPreferencesImpl keeps listeners weakly; retain this one explicitly.
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
            pendingSync = EXECUTOR.schedule(Os4LauncherRootPatcherSafe::syncNow, delayMs, TimeUnit.MILLISECONDS);
        }
    }

    private static void syncNow() {
        boolean hotseat = PrefsBridge.getBoolean(KEY_HOTSEAT);
        boolean grid = PrefsBridge.getBoolean(KEY_GRID);
        boolean icon = PrefsBridge.getBoolean(KEY_ICON_ENABLE);
        int cellX = clamp(PrefsBridge.getInt(KEY_CELL_X, 4), 3, 9);
        int cellY = clamp(PrefsBridge.getInt(KEY_CELL_Y, 6), 4, 13);
        int iconSize = clamp(PrefsBridge.getInt(KEY_ICON_SIZE, 182), 50, 360);

        String daemon = buildDaemonScript(hotseat, grid, icon, cellX, cellY, iconSize);
        String controller = buildControllerScript(
            daemon,
            hotseat || grid || icon,
            grid,
            cellX,
            cellY
        );
        String output = runAsRoot(controller, 15_000L);
        if (output == null) {
            Log.e(TAG, "root controller failed or timed out");
            return;
        }
        Log.i(
            TAG,
            "root patcher synced: hotseat=" + hotseat +
                " grid=" + grid + "(" + cellX + "x" + cellY + ")" +
                " icon=" + icon + "(" + iconSize + ") output=" + output.trim()
        );
    }

    private static String buildControllerScript(
        String daemon,
        boolean anyEnabled,
        boolean gridEnabled,
        int cellX,
        int cellY
    ) {
        StringBuilder sh = new StringBuilder(daemon.length() + 4096);
        sh.append("SCRIPT='").append(DAEMON_PATH).append("'\n");
        sh.append("PIDFILE='").append(PID_PATH).append("'\n");
        sh.append("LOGFILE='").append(LOG_PATH).append("'\n");
        sh.append("stop_old_daemon(){\n");
        sh.append("  if [ -s \"$PIDFILE\" ]; then\n");
        sh.append("    old=$(cat \"$PIDFILE\" 2>/dev/null)\n");
        sh.append("    if [ -n \"$old\" ] && [ -r \"/proc/$old/cmdline\" ]; then\n");
        sh.append("      cmd=$(tr '\\000' ' ' < \"/proc/$old/cmdline\" 2>/dev/null)\n");
        sh.append("      case \"$cmd\" in *hyperceiler_os4_patcher.sh*) kill \"$old\" 2>/dev/null ;; esac\n");
        sh.append("    fi\n");
        sh.append("  fi\n");
        sh.append("  rm -f \"$PIDFILE\"\n");
        sh.append("}\n");
        sh.append("update_grid_file(){\n");
        sh.append("  file=\"$1\"; gx=\"$2\"; gy=\"$3\"\n");
        sh.append("  [ -f \"$file\" ] || return 1\n");
        sh.append("  tmp=/data/local/tmp/hyperceiler_grid_$$.xml\n");
        sh.append("  awk -v gx=\"$gx\" -v gy=\"$gy\" '\n");
        sh.append("    BEGIN { fx=0; fy=0 }\n");
        sh.append("    /<int[[:space:]]+name=\"pref_key_cell_x\"/ { sub(/value=\"[^\"]*\"/, \"value=\\\"\" gx \"\\\"\"); fx=1 }\n");
        sh.append("    /<int[[:space:]]+name=\"pref_key_cell_y\"/ { sub(/value=\"[^\"]*\"/, \"value=\\\"\" gy \"\\\"\"); fy=1 }\n");
        sh.append("    /<\\/map>/ {\n");
        sh.append("      if (!fx) print \"    <int name=\\\"pref_key_cell_x\\\" value=\\\"\" gx \"\\\" />\"\n");
        sh.append("      if (!fy) print \"    <int name=\\\"pref_key_cell_y\\\" value=\\\"\" gy \"\\\" />\"\n");
        sh.append("    }\n");
        sh.append("    { print }\n");
        sh.append("  ' \"$file\" > \"$tmp\" || { rm -f \"$tmp\"; return 1; }\n");
        // Copy back into the existing inode so owner/mode/SELinux label stay intact.
        sh.append("  cat \"$tmp\" > \"$file\" || { rm -f \"$tmp\"; return 1; }\n");
        sh.append("  rm -f \"$tmp\"\n");
        sh.append("  return 0\n");
        sh.append("}\n");

        sh.append("stop_old_daemon\n");
        sh.append("sleep 0.05\n");

        if (gridEnabled) {
            sh.append("grid_updated=0\n");
            sh.append("for pref in /data/user/0/com.miui.home/shared_prefs/launcher_sharedpreference.xml /data/user_de/0/com.miui.home/shared_prefs/launcher_sharedpreference.xml; do\n");
            sh.append("  if update_grid_file \"$pref\" ").append(cellX).append(' ').append(cellY).append("; then\n");
            sh.append("    echo \"grid prefs updated: $pref -> ").append(cellX).append('x').append(cellY).append("\"\n");
            sh.append("    grid_updated=1\n");
            sh.append("  fi\n");
            sh.append("done\n");
            sh.append("[ $grid_updated -eq 0 ] && echo 'WARNING launcher_sharedpreference.xml not found'\n");
        }

        if (!anyEnabled) {
            sh.append("pids=$(pidof com.miui.home 2>/dev/null)\n");
            sh.append("[ -n \"$pids\" ] && kill -9 $pids 2>/dev/null\n");
            sh.append("echo 'OS4 patcher disabled; launcher restarted'\n");
            return sh.toString();
        }

        sh.append("cat > \"$SCRIPT\" <<'HYPERCEILER_OS4_EOF'\n");
        sh.append(daemon);
        if (!daemon.endsWith("\n")) sh.append('\n');
        sh.append("HYPERCEILER_OS4_EOF\n");
        sh.append("chmod 600 \"$SCRIPT\"\n");
        sh.append("rm -f \"$LOGFILE\"\n");
        sh.append("if command -v setsid >/dev/null 2>&1; then\n");
        sh.append("  setsid /system/bin/sh \"$SCRIPT\" >\"$LOGFILE\" 2>&1 < /dev/null &\n");
        sh.append("else\n");
        sh.append("  /system/bin/sh \"$SCRIPT\" >\"$LOGFILE\" 2>&1 < /dev/null &\n");
        sh.append("fi\n");
        sh.append("i=0\n");
        sh.append("while [ $i -lt 60 ] && [ ! -s \"$PIDFILE\" ]; do sleep 0.02; i=$((i+1)); done\n");
        sh.append("if [ ! -s \"$PIDFILE\" ]; then echo 'daemon failed to start'; exit 1; fi\n");
        // Reset the current process after the watcher and grid prefs are ready.
        sh.append("pids=$(pidof com.miui.home 2>/dev/null)\n");
        sh.append("[ -n \"$pids\" ] && kill -9 $pids 2>/dev/null\n");
        sh.append("echo 'daemon started pid='$(cat \"$PIDFILE\")\n");
        return sh.toString();
    }

    private static String buildDaemonScript(
        boolean hotseat,
        boolean grid,
        boolean icon,
        int cellX,
        int cellY,
        int iconSize
    ) {
        StringBuilder sh = new StringBuilder(8192);
        sh.append("#!/system/bin/sh\n");
        sh.append("PIDFILE='").append(PID_PATH).append("'\n");
        sh.append("echo $$ > \"$PIDFILE\"\n");
        sh.append("trap 'rm -f \"$PIDFILE\"' EXIT INT TERM\n");
        sh.append("BUILD_NOTE='").append(SUPPORTED_BUILD_NOTE_HEX).append("'\n");
        sh.append("BUILD_NOTE_RVA=").append(RVA_BUILD_NOTE).append("\n");
        sh.append("KNOWN_APK_OFFSET=").append(KNOWN_LIBAPP_APK_OFFSET).append("\n");
        sh.append("log(){ echo \"$(date '+%m-%d %H:%M:%S') $*\"; }\n");
        sh.append("read_hex(){ dd if=\"/proc/$1/mem\" bs=1 skip=\"$2\" count=\"$3\" 2>/dev/null | od -An -v -tx1 | tr -d ' \\n'; }\n");
        sh.append("verify_base(){ candidate=\"$1\"; note=$(read_hex \"$pid\" $((candidate + BUILD_NOTE_RVA)) 32); [ \"$note\" = \"$BUILD_NOTE\" ]; }\n");
        sh.append("find_libapp_base(){\n");
        sh.append("  target_pid=\"$1\"\n");
        // Fast path for the exact supplied APK: no forked dd probes until the
        // expected libapp mapping appears in /proc/<pid>/maps.
        sh.append("  while read -r range perms off dev inode path rest; do\n");
        sh.append("    [ -z \"$path\" ] && continue\n");
        sh.append("    case \"$path\" in *base.apk*) ;; *) continue ;; esac\n");
        sh.append("    off_dec=$((0x$off))\n");
        sh.append("    if [ \"$off_dec\" -eq \"$KNOWN_APK_OFFSET\" ]; then\n");
        sh.append("      start_hex=${range%%-*}; start=$((0x$start_hex))\n");
        sh.append("      if verify_base \"$start\"; then echo \"$start\"; return 0; fi\n");
        sh.append("    fi\n");
        sh.append("  done < \"/proc/$target_pid/maps\"\n");
        // Fallback for a repacked-but-identical library: test base.apk mapping
        // starts by their in-memory GNU note instead of trusting ZIP layout.
        sh.append("  while read -r range perms off dev inode path rest; do\n");
        sh.append("    [ -z \"$path\" ] && continue\n");
        sh.append("    case \"$path\" in *base.apk*) ;; *) continue ;; esac\n");
        sh.append("    start_hex=${range%%-*}; start=$((0x$start_hex))\n");
        sh.append("    if verify_base \"$start\"; then echo \"$start\"; return 0; fi\n");
        sh.append("  done < \"/proc/$target_pid/maps\"\n");
        sh.append("  return 1\n");
        sh.append("}\n");
        sh.append("patch_one(){\n");
        sh.append("  name=\"$1\"; addr=\"$2\"; expected=\"$3\"; desired=\"$4\"; escaped=\"$5\"; len=\"$6\"\n");
        sh.append("  current=$(read_hex \"$pid\" \"$addr\" \"$len\")\n");
        sh.append("  if [ \"$current\" = \"$desired\" ]; then log \"$name already patched\"; return 0; fi\n");
        sh.append("  if [ \"$current\" != \"$expected\" ]; then log \"ERROR $name prologue mismatch: $current\"; return 1; fi\n");
        sh.append("  printf '%b' \"$escaped\" | dd of=\"/proc/$pid/mem\" bs=1 seek=\"$addr\" conv=notrunc 2>/dev/null || { log \"ERROR $name write failed\"; return 1; }\n");
        sh.append("  verify=$(read_hex \"$pid\" \"$addr\" \"$len\")\n");
        sh.append("  if [ \"$verify\" != \"$desired\" ]; then log \"ERROR $name verify failed: $verify\"; return 1; fi\n");
        sh.append("  log \"$name patched at $addr\"\n");
        sh.append("  return 0\n");
        sh.append("}\n");
        sh.append("patch_pid(){\n");
        sh.append("  pid=\"$1\"\n");
        sh.append("  [ -r \"/proc/$pid/maps\" ] || return 2\n");
        sh.append("  base=$(find_libapp_base \"$pid\") || return 2\n");
        sh.append("  [ -n \"$base\" ] || return 2\n");
        sh.append("  log \"supported libapp.so pid=$pid base=$base\"\n");
        sh.append("  kill -STOP \"$pid\" 2>/dev/null || { log 'ERROR cannot stop launcher'; return 3; }\n");
        sh.append("  rc=0\n");

        if (hotseat) {
            appendPatch(sh, "DeviceConfig.hotSeatMaxCount", RVA_HOTSEAT_MAX, EXPECT_HOTSEAT, returnIntX0(99));
        }
        if (grid) {
            appendPatch(sh, "DeviceConfig.cellCountXMax", RVA_CELL_X_MAX, EXPECT_CONFIG_GETTER, returnSmiX0(9));
            appendPatch(sh, "DeviceConfig.cellCountXMin", RVA_CELL_X_MIN, EXPECT_CONFIG_GETTER, returnSmiX0(3));
            appendPatch(sh, "DeviceConfig.cellCountXDef", RVA_CELL_X_DEF, EXPECT_CONFIG_GETTER, returnSmiX0(cellX));
            appendPatch(sh, "DeviceConfig.cellCountYDef", RVA_CELL_Y_DEF, EXPECT_CONFIG_GETTER, returnSmiX0(cellY));
        }
        if (icon) {
            appendPatch(sh, "_IconConfig.getIconSize", RVA_ICON_SIZE, EXPECT_ICON, returnDoubleD0(iconSize));
        }

        sh.append("  kill -CONT \"$pid\" 2>/dev/null\n");
        sh.append("  return $rc\n");
        sh.append("}\n");
        sh.append("log 'daemon started: hotseat=").append(hotseat)
            .append(" grid=").append(grid).append(" cell=").append(cellX).append('x').append(cellY)
            .append(" icon=").append(icon).append(" iconSize=").append(iconSize).append("'\n");
        sh.append("last_pid=''\n");
        sh.append("retry_pid=''\n");
        sh.append("retry_count=0\n");
        sh.append("while :; do\n");
        sh.append("  pid=$(pidof com.miui.home 2>/dev/null | awk '{print $1}')\n");
        sh.append("  if [ -z \"$pid\" ]; then last_pid=''; retry_pid=''; retry_count=0; sleep 0.005; continue; fi\n");
        sh.append("  if [ \"$pid\" = \"$last_pid\" ]; then sleep 0.20; continue; fi\n");
        sh.append("  if [ \"$pid\" != \"$retry_pid\" ]; then retry_pid=\"$pid\"; retry_count=0; fi\n");
        sh.append("  patch_pid \"$pid\"; result=$?\n");
        sh.append("  if [ $result -eq 0 ]; then last_pid=\"$pid\"; retry_count=0; sleep 0.20; continue; fi\n");
        sh.append("  if [ $result -eq 2 ]; then\n");
        sh.append("    retry_count=$((retry_count+1))\n");
        sh.append("    if [ $retry_count -ge 1000 ]; then log \"ERROR libapp.so/build-id not found for pid=$pid\"; last_pid=\"$pid\"; fi\n");
        sh.append("    sleep 0.002\n");
        sh.append("    continue\n");
        sh.append("  fi\n");
        sh.append("  last_pid=\"$pid\"\n");
        sh.append("  sleep 0.20\n");
        sh.append("done\n");
        return sh.toString();
    }

    private static void appendPatch(
        StringBuilder sh,
        String name,
        long rva,
        byte[] expected,
        byte[] desired
    ) {
        sh.append("  patch_one ")
            .append(shellSingleQuote(name)).append(' ')
            .append("$((base + ").append(rva).append(")) ")
            .append(shellSingleQuote(toHex(expected))).append(' ')
            .append(shellSingleQuote(toHex(desired))).append(' ')
            .append(shellSingleQuote(toPrintfEscapes(desired))).append(' ')
            .append(desired.length)
            .append(" || rc=1\n");
    }

    private static byte[] returnIntX0(int value) {
        return instructions(
            0xD2800000 | ((value & 0xFFFF) << 5),
            0xD65F03C0
        );
    }

    private static byte[] returnSmiX0(int value) {
        return returnIntX0(value << 1);
    }

    private static byte[] returnDoubleD0(int value) {
        return instructions(
            0x52800000 | ((value & 0xFFFF) << 5),
            0x1E620000,
            0xD65F03C0
        );
    }

    private static byte[] instructions(int... words) {
        byte[] result = new byte[words.length * 4];
        for (int i = 0; i < words.length; i++) {
            int word = words[i];
            int offset = i * 4;
            result[offset] = (byte) word;
            result[offset + 1] = (byte) (word >>> 8);
            result[offset + 2] = (byte) (word >>> 16);
            result[offset + 3] = (byte) (word >>> 24);
        }
        return result;
    }

    private static String toHex(byte[] bytes) {
        StringBuilder out = new StringBuilder(bytes.length * 2);
        for (byte value : bytes) out.append(String.format(Locale.ROOT, "%02x", value & 0xFF));
        return out.toString();
    }

    private static String toPrintfEscapes(byte[] bytes) {
        StringBuilder out = new StringBuilder(bytes.length * 4);
        for (byte value : bytes) {
            out.append('\\');
            String octal = Integer.toOctalString(value & 0xFF);
            for (int pad = octal.length(); pad < 3; pad++) out.append('0');
            out.append(octal);
        }
        return out.toString();
    }

    private static byte[] hex(String value) {
        if ((value.length() & 1) != 0) throw new IllegalArgumentException("odd hex length");
        byte[] result = new byte[value.length() / 2];
        for (int i = 0; i < result.length; i++) {
            int hi = Character.digit(value.charAt(i * 2), 16);
            int lo = Character.digit(value.charAt(i * 2 + 1), 16);
            if (hi < 0 || lo < 0) throw new IllegalArgumentException("invalid hex");
            result[i] = (byte) ((hi << 4) | lo);
        }
        return result;
    }

    private static String shellSingleQuote(String value) {
        return "'" + value.replace("'", "'\\''") + "'";
    }

    private static int clamp(int value, int min, int max) {
        return Math.max(min, Math.min(max, value));
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

            StringBuilder output = new StringBuilder();
            try (BufferedReader reader = new BufferedReader(
                new InputStreamReader(process.getInputStream(), StandardCharsets.UTF_8)
            )) {
                String line;
                while ((line = reader.readLine()) != null) {
                    if (output.length() < 8192) output.append(line).append('\n');
                }
            }

            if (!process.waitFor(timeoutMs, TimeUnit.MILLISECONDS)) {
                process.destroyForcibly();
                return null;
            }
            if (process.exitValue() != 0) {
                Log.e(TAG, "root command exit=" + process.exitValue() + " output=" + output);
                return null;
            }
            return output.toString();
        } catch (Throwable error) {
            Log.e(TAG, "runAsRoot failed", error);
            if (process != null) process.destroyForcibly();
            return null;
        }
    }
}
