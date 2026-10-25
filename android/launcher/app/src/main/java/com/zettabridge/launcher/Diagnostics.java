package com.zettabridge.launcher;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.os.Build;
import java.io.InputStream;
import java.util.List;
import android.util.Log;

import com.zettabridge.core.ZBridge;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * Error reports that do not depend on logcat: some ROMs (OxygenOS) drop third-party app logs.
 * The full stack trace goes to the clipboard and to
 * /sdcard/Android/data/com.zettabridge.launcher/files/zb-errors.txt (appended).
 *
 * <p>The translator's own runtime report lands next to it as zb-runtime-report.txt, rewritten by
 * the :guest process whenever it changes, so it survives a :guest process that dies silently.
 */
final class Diagnostics {
    private static final String TAG = "zb-launcher";
    private static final String REPORT_NAME = "zb-runtime-report.txt";

    private Diagnostics() {}

    /** The runtime report file, or null when external storage is unavailable. */
    static File runtimeReportFile(Context context) {
        File dir = context.getExternalFilesDir(null);
        return dir == null ? null : new File(dir, REPORT_NAME);
    }

    /**
     * Makes the :guest process persist its runtime report. Called once, before plugin code runs;
     * failures are not fatal, the run simply leaves no report behind.
     */
    static void startRuntimeReport(Context context) {
        File file = runtimeReportFile(context);
        if (file == null) {
            Log.w(TAG, "no external files dir: the runtime report is not persisted");
            return;
        }
        try {
            if (!ZBridge.setReportFile(file.getAbsolutePath())) {
                Log.w(TAG, "cannot persist the runtime report to " + file);
            }
        } catch (Throwable t) {
            Log.w(TAG, "cannot start the runtime report: " + t);
        }
    }

    /** The last run report, or null when no run has written one. */
    static String readRuntimeReport(Context context) {
        File file = runtimeReportFile(context);
        if (file == null || !file.isFile()) return null;
        try {
            String text = new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
            return text.isEmpty() ? null : text;
        } catch (IOException | RuntimeException e) {
            Log.w(TAG, "cannot read the runtime report: " + e);
            return null;
        }
    }

    /** ADB-free diagnostics from Java uncaught handlers and Android process exit history. */
    static String readCrashDiagnostics(Context context) {
        StringBuilder out = new StringBuilder();
        File dir = context.getExternalFilesDir(null);
        File errors = dir == null ? null : new File(dir, "zb-errors.txt");
        if (errors != null && errors.isFile()) {
            try {
                byte[] data = Files.readAllBytes(errors.toPath());
                int start = Math.max(0, data.length - 24000);
                out.append("\n--- Java crash/error log (most recent bytes) ---\n");
                out.append(new String(data, start, data.length - start, StandardCharsets.UTF_8));
            } catch (IOException | RuntimeException e) {
                out.append("\nerror-log-read-failed: ").append(e).append('\n');
            }
        } else {
            out.append("\nJava crash log: (none)\n");
        }
        if (Build.VERSION.SDK_INT >= 30) {
            out.append("\n--- Android recent process exits ---\n");
            try {
                ActivityManager am = (ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
                List<ApplicationExitInfo> exits = am == null ? null :
                        am.getHistoricalProcessExitReasons(context.getPackageName(), 0, 8);
                if (exits == null || exits.isEmpty()) {
                    out.append("(none)\n");
                } else {
                    for (ApplicationExitInfo exit : exits) {
                        out.append("process: ").append(exit.getProcessName())
                                .append(" reason=").append(exit.getReason())
                                .append(" status=").append(exit.getStatus())
                                .append(" time=").append(exit.getTimestamp())
                                .append(" description=").append(exit.getDescription())
                                .append('\n');
                        if (exit.getReason() == ApplicationExitInfo.REASON_CRASH_NATIVE ||
                                exit.getReason() == ApplicationExitInfo.REASON_ANR) {
                            try (InputStream trace = exit.getTraceInputStream()) {
                                if (trace != null) {
                                    byte[] data = new byte[12000];
                                    int n = trace.read(data);
                                    if (n > 0) {
                                        out.append("exit-trace (truncated):\n")
                                                .append(new String(data, 0, n, StandardCharsets.UTF_8))
                                                .append('\n');
                                    }
                                }
                            } catch (IOException | RuntimeException e) {
                                out.append("exit-trace-unavailable: ").append(e).append('\n');
                            }
                        }
                    }
                }
            } catch (RuntimeException e) {
                out.append("process-exit-history-unavailable: ").append(e).append('\n');
            }
        }
        return out.toString();
    }

    /** Copies text to the clipboard under `label`; false when the clipboard is unavailable. */
    static boolean copy(Context context, String label, String text) {
        try {
            ClipboardManager cm = (ClipboardManager) context.getSystemService(Context.CLIPBOARD_SERVICE);
            if (cm == null) return false;
            cm.setPrimaryClip(ClipData.newPlainText(label, text));
            return true;
        } catch (RuntimeException e) {
            Log.w(TAG, "cannot copy to the clipboard: " + e);
            return false;
        }
    }

    /** Records an error; copies it to the clipboard when copy is true. Returns the report text. */
    static String report(Context context, String what, Throwable t, boolean copy) {
        String stamp = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(new Date());
        String text = stamp + " " + what + "\n" + Log.getStackTraceString(t);
        Log.e(TAG, what, t);
        appendToFile(context, text);
        if (copy) copy(context, "ZettaBridge error", text);
        return text;
    }

    /** Uncaught exceptions of a process are appended to the error file before the default handler runs. */
    static void installCrashRecorder(Context context) {
        final Context app = context.getApplicationContext() != null ? context.getApplicationContext() : context;
        final Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, t) -> {
            try {
                appendToFile(app, new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(new Date())
                        + " uncaught in thread " + thread.getName() + "\n" + Log.getStackTraceString(t));
            } catch (Throwable ignored) {
                // never mask the original crash
            }
            if (previous != null) previous.uncaughtException(thread, t);
        });
    }

    private static void appendToFile(Context context, String text) {
        try {
            File dir = context.getExternalFilesDir(null);
            if (dir == null) return;
            try (FileOutputStream out = new FileOutputStream(new File(dir, "zb-errors.txt"), true)) {
                out.write((text + "\n").getBytes(StandardCharsets.UTF_8));
            }
        } catch (Exception e) {
            Log.w(TAG, "cannot write the error file: " + e);
        }
    }
}
