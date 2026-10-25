package com.zettabridge.launcher;

import android.app.Activity;
import android.content.res.TypedArray;
import android.graphics.Insets;
import android.os.Build;
import android.util.Log;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

/**
 * Applies a plugin activity's window decoration the way the plugin's own theme asks for it.
 *
 * Two things do not happen by themselves for a plugin. The activity's window was created from the
 * launcher's stub theme before the plugin theme was swapped in, so an action bar or title can
 * survive; and a fullscreen guest hides the navigation bar with setSystemUiVisibility, which newer
 * Android releases ignore. Both are read from the plugin theme and applied here, so nothing is
 * hidden that the guest did not ask to hide.
 */
final class GuestWindowStyle {
    private static final String TAG = "zb-launcher";

    private GuestWindowStyle() {}

    /**
     * The theme the framework would pick for an app that declares none, by its targetSdk. The old
     * default is a dark, title-bar theme; using a modern light one instead left a grey action bar
     * above games that never asked for one.
     */
    static int defaultTheme(int targetSdk) {
        if (targetSdk >= Build.VERSION_CODES.ICE_CREAM_SANDWICH) return android.R.style.Theme_DeviceDefault;
        if (targetSdk >= Build.VERSION_CODES.HONEYCOMB) return android.R.style.Theme_Holo;
        return android.R.style.Theme;
    }

    /**
     * Runs after the plugin's own onCreate. Old games ask for fullscreen there, by the time our
     * theme pass is long over, so honour what the window says now.
     */
    static void afterCreate(Activity activity) {
        Window window = activity.getWindow();
        if (window == null) return;
        final int flags = window.getAttributes().flags;
        if ((flags & WindowManager.LayoutParams.FLAG_FULLSCREEN) == 0) {
            keepBelowSystemBars(activity);
            return;
        }
        if (activity.getActionBar() != null) activity.getActionBar().hide();
        hideSystemBars(activity);
        final View decor = window.getDecorView();
        decor.getViewTreeObserver().addOnWindowFocusChangeListener(focused -> {
            if (focused) hideSystemBars(activity);
        });
    }

    static void apply(Activity activity, boolean translated) {
        Window window = activity.getWindow();
        if (window == null) return;
        boolean noTitle = false;
        boolean actionBar = true;
        boolean fullscreen = false;
        try {
            TypedArray a = activity.getTheme().obtainStyledAttributes(new int[] {
                android.R.attr.windowNoTitle,
                android.R.attr.windowActionBar,
                android.R.attr.windowFullscreen,
            });
            noTitle = a.getBoolean(0, false);
            actionBar = a.getBoolean(1, true);
            fullscreen = a.getBoolean(2, false);
            a.recycle();
        } catch (RuntimeException e) {
            Log.w(TAG, "cannot read the plugin window theme: " + e);
        }

        // Requesting it here is the only thing that works: the feature has to be set before the
        // plugin's onCreate calls setContentView. A translated plugin is an old game, and those
        // expect no title bar at all; a 64-bit plugin keeps whatever its theme asks for.
        if (noTitle || !actionBar || translated) {
            try {
                window.requestFeature(Window.FEATURE_NO_TITLE);
            } catch (RuntimeException e) {
                Log.w(TAG, "cannot drop the title bar: " + e);
            }
            if (activity.getActionBar() != null) activity.getActionBar().hide();
        }
        if (!fullscreen) return;

        window.setFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN,
                WindowManager.LayoutParams.FLAG_FULLSCREEN);
        hideSystemBars(activity);
        // The bars come back on every focus change (a dialog, the recents switcher), and the
        // guest's own setSystemUiVisibility no longer brings them down, so re-apply.
        final View decor = window.getDecorView();
        decor.getViewTreeObserver().addOnWindowFocusChangeListener(focused -> {
            if (focused) hideSystemBars(activity);
        });
    }

    /**
     * Android 15+ draws every window of a targetSdk 35 app edge to edge, and the launcher owns the
     * plugin's window. A plugin built before that rule (StyleTap) lays its screen out from the top
     * of the window, so its top row went under the status bar. For a non-fullscreen plugin that
     * targets SDK 34 or lower, pad the content view by the system bar and cutout insets, which is
     * where the system would have placed it. Newer plugins handle insets themselves.
     */
    static void keepBelowSystemBars(Activity activity) {
        if (Build.VERSION.SDK_INT < 35) return;
        if (!needsInsetPadding(activity.getApplicationInfo().targetSdkVersion)) return;
        final View content = activity.findViewById(android.R.id.content);
        if (content == null) return;
        final boolean[] noted = {false};
        content.setOnApplyWindowInsetsListener((v, insets) -> {
            Insets bars = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            v.setPadding(bars.left, bars.top, bars.right, bars.bottom);
            if (!noted[0]) {
                noted[0] = true;
                Diagnostics.note(activity, "window-insets: top=" + bars.top + " bottom=" + bars.bottom
                        + " left=" + bars.left + " right=" + bars.right + " applied=padding");
            }
            return insets;
        });
        content.requestApplyInsets();
    }

    /** Plugins targeting SDK 34 or lower predate enforced edge-to-edge. */
    static boolean needsInsetPadding(int targetSdk) {
        return targetSdk < 35;
    }

    @SuppressWarnings("deprecation")
    private static void hideSystemBars(Activity activity) {
        Window window = activity.getWindow();
        if (window == null) return;
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                window.setDecorFitsSystemWindows(false);
                WindowInsetsController controller = window.getInsetsController();
                if (controller != null) {
                    controller.hide(WindowInsets.Type.systemBars());
                    controller.setSystemBarsBehavior(
                            WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                }
            } else {
                window.getDecorView().setSystemUiVisibility(
                        View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                                | View.SYSTEM_UI_FLAG_FULLSCREEN
                                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "cannot hide the system bars: " + e);
        }
    }
}
