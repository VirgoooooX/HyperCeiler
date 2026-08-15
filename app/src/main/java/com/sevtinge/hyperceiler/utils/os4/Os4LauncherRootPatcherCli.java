package com.sevtinge.hyperceiler.utils.os4;

/**
 * Tiny command-line entry used through root app_process.
 *
 * "watch" starts the resident native watcher before com.miui.home is
 * restarted, allowing the Rust launcher to be patched before DeviceParam and
 * GridConfig are initialized. The legacy one-shot mode is kept for diagnostics.
 */
public final class Os4LauncherRootPatcherCli {
    private Os4LauncherRootPatcherCli() {}

    private static native int nativePatch(
        int pid,
        boolean hotseat,
        boolean grid,
        int cellX,
        int cellY,
        boolean icon,
        int iconSize
    );

    private static native int nativeWatch(
        boolean hotseat,
        boolean grid,
        int cellX,
        int cellY,
        boolean icon,
        int iconSize
    );

    public static void main(String[] args) {
        if (args.length == 8 && "watch".equals(args[0])) {
            runWatcher(args);
            return;
        }
        runOneShot(args);
    }

    private static void runWatcher(String[] args) {
        if (!loadLibrary(args[1])) return;
        try {
            int result = nativeWatch(
                Boolean.parseBoolean(args[2]),
                Boolean.parseBoolean(args[3]),
                Integer.parseInt(args[4]),
                Integer.parseInt(args[5]),
                Boolean.parseBoolean(args[6]),
                Integer.parseInt(args[7])
            );
            System.out.println("native watcher result=" + result);
            System.exit(result);
        } catch (Throwable t) {
            System.out.println("ERROR native watcher exception: " + t);
            System.exit(66);
        }
    }

    private static void runOneShot(String[] args) {
        if (args.length != 8) {
            System.out.println(
                "ERROR usage: watch <lib> <hotseat> <grid> <x> <y> <icon> <iconSize> " +
                    "OR <lib> <pid> <hotseat> <grid> <x> <y> <icon> <iconSize>"
            );
            System.exit(64);
            return;
        }
        if (!loadLibrary(args[0])) return;
        try {
            int result = nativePatch(
                Integer.parseInt(args[1]),
                Boolean.parseBoolean(args[2]),
                Boolean.parseBoolean(args[3]),
                Integer.parseInt(args[4]),
                Integer.parseInt(args[5]),
                Boolean.parseBoolean(args[6]),
                Integer.parseInt(args[7])
            );
            System.out.println("native helper result=" + result);
            System.exit(result);
        } catch (Throwable t) {
            System.out.println("ERROR native helper exception: " + t);
            System.exit(66);
        }
    }

    private static boolean loadLibrary(String path) {
        try {
            System.out.println("native helper loading: " + path);
            System.load(path);
            return true;
        } catch (Throwable t) {
            System.out.println("ERROR native helper load failed: " + t);
            System.exit(65);
            return false;
        }
    }
}
