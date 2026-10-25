package com.zettabridge.launcher;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.IBinder;
import android.content.res.Resources;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import java.util.ArrayList;
import android.os.PersistableBundle;
import android.util.Log;

import java.lang.reflect.Method;

/**
 * Replaces ActivityThread.mInstrumentation in the :guest process.
 * - execStartActivity*: intents naming a plugin activity are rewritten to a manifest stub.
 * - newActivity: a stub intent instantiates the plugin activity from the plugin class loader,
 *   so the framework attaches it with the stub's real token and window.
 * - callActivityOnCreate: swaps in the plugin context, resources, theme and original intent
 *   before the plugin's onCreate runs.
 * Everything else delegates to the original instance, which the framework initialized.
 */
final class GuestInstrumentation extends Instrumentation {
    private static final String TAG = "zb-launcher";

    private final GuestRuntime runtime;
    private final Instrumentation base;
    private Method execStartActivity;
    private Method execStartActivityFromString;
    private Method execStartActivities;

    GuestInstrumentation(GuestRuntime runtime, Instrumentation base) {
        this.runtime = runtime;
        this.base = base;
    }

    @Override
    public Activity newActivity(ClassLoader cl, String className, Intent intent)
            throws InstantiationException, IllegalAccessException, ClassNotFoundException {
        Activity plugin = runtime.instantiatePluginActivity(intent);
        return plugin != null ? plugin : base.newActivity(cl, className, intent);
    }

    @Override
    public void callActivityOnCreate(Activity activity, Bundle icicle) {
        runtime.prepareActivity(activity, icicle);
        try {
            base.callActivityOnCreate(activity, icicle);
        } catch (RuntimeException | Error error) {
            diagnoseViewCrash(activity, error);
            throw error;
        }
        GuestWindowStyle.afterCreate(activity);
    }

    @Override
    public void callActivityOnCreate(Activity activity, Bundle icicle, PersistableBundle persistentState) {
        runtime.prepareActivity(activity, icicle);
        try {
            base.callActivityOnCreate(activity, icicle, persistentState);
        } catch (RuntimeException | Error error) {
            diagnoseViewCrash(activity, error);
            throw error;
        }
        GuestWindowStyle.afterCreate(activity);
    }

    /**
     * Record the plugin resource/view mismatch before ActivityThread terminates the guest.
     * Does not mutate the Activity or attempt to fabricate missing views.
     */
    private void diagnoseViewCrash(Activity activity, Throwable crash) {
        if (!"com.StyleTap.StyleTap".equals(activity.getPackageName())
                && !activity.getClass().getName().startsWith("com.StyleTap.StyleTap.")) return;
        StringBuilder detail = new StringBuilder("StyleTap view diagnostics: ");
        try {
            Resources res = activity.getResources();
            String pkg = activity.getClass().getPackage().getName();
            int id = res.getIdentifier("dummyEditField", "id", pkg);
            detail.append("activity=").append(activity.getClass().getName())
                    .append(" contextPackage=").append(activity.getPackageName())
                    .append(" resourceClass=").append(res.getClass().getName())
                    .append(" dummyEditFieldId=0x").append(Integer.toHexString(id));
            View view = id == 0 ? null : activity.findViewById(id);
            detail.append(" found=").append(view == null ? "null" : view.getClass().getName());
            Window window = activity.getWindow();
            View decor = window == null ? null : window.peekDecorView();
            detail.append(" decor=").append(decor == null ? "null" : decor.getClass().getName());
            if (decor != null) {
                ArrayList<View> pending = new ArrayList<>();
                pending.add(decor);
                int count = 0;
                while (!pending.isEmpty() && count < 150) {
                    View v = pending.remove(pending.size() - 1);
                    count++;
                    int vid = v.getId();
                    if (vid != View.NO_ID) {
                        String name;
                        try { name = res.getResourceEntryName(vid); }
                        catch (Resources.NotFoundException ex) { name = "unknown"; }
                        if (name.toLowerCase(java.util.Locale.ROOT).contains("dummy")
                                || v.getClass().getName().contains("DummyEditText")) {
                            detail.append(" candidate=").append(v.getClass().getName())
                                    .append("/0x").append(Integer.toHexString(vid))
                                    .append("/").append(name);
                        }
                    }
                    if (v instanceof ViewGroup) {
                        ViewGroup group = (ViewGroup) v;
                        for (int i = 0; i < group.getChildCount(); ++i) pending.add(group.getChildAt(i));
                    }
                }
                detail.append(" scanned=").append(count);
            }
        } catch (Throwable e) {
            detail.append(" diagnostic-error=").append(e);
        }
        Diagnostics.report(activity, detail.toString(), crash, false);
    }

    @Override
    public void callActivityOnDestroy(Activity activity) {
        base.callActivityOnDestroy(activity);
        runtime.onActivityDestroyed(activity);
    }

    // Hidden framework entry points of Activity.startActivity and Context.startActivity. They are
    // not in the SDK stubs, so these only override at runtime; the originals are called on base.

    public ActivityResult execStartActivity(Context who, IBinder contextThread, IBinder token, Activity target,
                                            Intent intent, int requestCode, Bundle options) {
        try {
            if (execStartActivity == null) {
                execStartActivity = Reflect.method(Instrumentation.class, "execStartActivity", Context.class,
                        IBinder.class, IBinder.class, Activity.class, Intent.class, int.class, Bundle.class);
            }
        } catch (NoSuchMethodException e) {
            throw new IllegalStateException("Instrumentation.execStartActivity not found", e);
        }
        Intent routed = runtime.route(intent);
        return (ActivityResult) Reflect.invoke(execStartActivity, base, who, contextThread, token, target, routed,
                requestCode, options);
    }

    public ActivityResult execStartActivity(Context who, IBinder contextThread, IBinder token, String target,
                                            Intent intent, int requestCode, Bundle options) {
        try {
            if (execStartActivityFromString == null) {
                execStartActivityFromString = Reflect.method(Instrumentation.class, "execStartActivity", Context.class,
                        IBinder.class, IBinder.class, String.class, Intent.class, int.class, Bundle.class);
            }
        } catch (NoSuchMethodException e) {
            throw new IllegalStateException("Instrumentation.execStartActivity(String) not found", e);
        }
        Intent routed = runtime.route(intent);
        return (ActivityResult) Reflect.invoke(execStartActivityFromString, base, who, contextThread, token, target,
                routed, requestCode, options);
    }

    public void execStartActivities(Context who, IBinder contextThread, IBinder token, Activity target,
                                    Intent[] intents, Bundle options) {
        try {
            if (execStartActivities == null) {
                execStartActivities = Reflect.method(Instrumentation.class, "execStartActivities", Context.class,
                        IBinder.class, IBinder.class, Activity.class, Intent[].class, Bundle.class);
            }
        } catch (NoSuchMethodException e) {
            throw new IllegalStateException("Instrumentation.execStartActivities not found", e);
        }
        Intent[] routed = new Intent[intents.length];
        for (int i = 0; i < intents.length; i++) routed[i] = runtime.route(intents[i]);
        Log.d(TAG, "execStartActivities: " + intents.length + " intents");
        Reflect.invoke(execStartActivities, base, who, contextThread, token, target, routed, options);
    }
}
