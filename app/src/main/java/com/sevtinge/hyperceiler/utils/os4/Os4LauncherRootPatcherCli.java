package com.sevtinge.hyperceiler.utils.os4;

/**
 * Tiny command-line entry used by the root watcher through app_process.
 * It deliberately avoids all HyperCeiler app/runtime dependencies so it can
 * be started as uid 0 with only the module APK on CLASSPATH.
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

    public static void main(String[] args) {
        if (args.length != 8) {
            System.out.println("ERROR native helper usage: <lib> <pid> <hotseat> <grid> <x> <y> <icon> <iconSize>");
            System.exit(64);
            return;
        }

        try {
            System.out.println("native helper loading: " + args[0]);
            System.load(args[0]);
        } catch (Throwable t) {
            System.out.println("ERROR native helper load failed: " + t);
            System.exit(65);
            return;
        }

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
}
