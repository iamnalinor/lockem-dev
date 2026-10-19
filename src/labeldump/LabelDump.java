package com.lockem;

import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.Drawable;
import android.os.Looper;

import java.io.File;
import java.io.FileOutputStream;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;

/**
 * Tiny helper run via `app_process` (the same mechanism `pm`/`am`/`cmd` use) to reach
 * PackageManager, which a plain root WebUI shell (ksu.exec) cannot call. Prints one
 * "packageName\tlabel" line per app and renders each app icon to a small cached PNG.
 *
 * Usage:  app_process /system/bin com.lockem.LabelDump <cacheDir> [pkg ...]
 *   - cacheDir: where to write <pkg>.png icons (created if missing)
 *   - pkg ...:  restrict to these packages; if omitted, dumps all installed apps
 *
 * Output goes to stdout as UTF-8; icon failures are non-fatal (the label line is still
 * printed, and the WebUI falls back to the package name / no icon).
 */
public final class LabelDump {

    private static final int ICON_PX = 48;
    // include hidden (locked) + disabled apps so their label/icon still resolve
    private static final int MATCH_FLAGS = 0x00002000 /* MATCH_UNINSTALLED_PACKAGES */
            | 0x00000200 /* MATCH_DISABLED_COMPONENTS */;

    public static void main(String[] args) {
        try {
            run(args);
        } catch (Throwable t) {
            System.err.println("labeldump: " + t);
            System.exit(1);
        }
    }

    private static void run(String[] args) throws Exception {
        String cacheDir = (args.length > 0) ? args[0] : "/data/adb/lockem/cache/icons";
        List<String> only = new ArrayList<>();
        for (int i = 1; i < args.length; i++) only.add(args[i]);

        // app_process has no Looper; ActivityThread.systemMain() needs the main one.
        if (Looper.getMainLooper() == null) Looper.prepareMainLooper();

        // Hidden-API (reflection): the shell-spawned app_process is exempt from the
        // hidden-API blocklist. This is the one line to revisit on a future Android.
        Class<?> atClass = Class.forName("android.app.ActivityThread");
        Method systemMain = atClass.getMethod("systemMain");
        Object at = systemMain.invoke(null);
        Method getSystemContext = atClass.getMethod("getSystemContext");
        Context ctx = (Context) getSystemContext.invoke(at);
        PackageManager pm = ctx.getPackageManager();

        File dir = new File(cacheDir);
        dir.mkdirs();

        List<ApplicationInfo> apps = new ArrayList<>();
        if (only.isEmpty()) {
            apps.addAll(pm.getInstalledApplications(MATCH_FLAGS));
        } else {
            for (String pkg : only) {
                try {
                    apps.add(pm.getApplicationInfo(pkg, MATCH_FLAGS));
                } catch (Throwable ignored) {
                    // not found (e.g. truly uninstalled) — WebUI falls back to the name
                }
            }
        }

        StringBuilder sb = new StringBuilder(apps.size() * 48);
        for (ApplicationInfo ai : apps) {
            String pkg = ai.packageName;
            String label;
            try {
                CharSequence cs = pm.getApplicationLabel(ai);
                label = (cs == null) ? pkg : cs.toString();
            } catch (Throwable t) {
                label = pkg;
            }
            label = label.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ').trim();
            if (label.isEmpty()) label = pkg;
            sb.append(pkg).append('\t').append(label).append('\n');

            File out = new File(dir, pkg + ".png");
            if (out.exists() && out.length() > 0) continue; // cached
            try {
                Drawable d = pm.getApplicationIcon(ai);
                Bitmap bmp = render(d);
                File tmp = new File(dir, pkg + ".png.tmp");
                try (FileOutputStream fos = new FileOutputStream(tmp)) {
                    bmp.compress(Bitmap.CompressFormat.PNG, 100, fos);
                    fos.flush();
                }
                if (!tmp.renameTo(out)) tmp.delete();
            } catch (Throwable ignored) {
                // icon render/write failed — leave it; label line already emitted
            }
        }
        System.out.print(sb);
        System.out.flush();
    }

    private static Bitmap render(Drawable d) {
        Bitmap bmp = Bitmap.createBitmap(ICON_PX, ICON_PX, Bitmap.Config.ARGB_8888);
        Canvas c = new Canvas(bmp);
        d.setBounds(0, 0, ICON_PX, ICON_PX);
        d.draw(c);
        return bmp;
    }

    private LabelDump() {}
}
