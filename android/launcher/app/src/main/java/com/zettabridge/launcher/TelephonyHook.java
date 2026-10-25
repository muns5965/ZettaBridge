package com.zettabridge.launcher;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.IBinder;
import android.util.Log;

import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.Map;

/**
 * Restores the pre-Android-10 answer of TelephonyManager.getDeviceId() for legacy plugins.
 *
 * A plugin that targets SDK 28 or lower and declares READ_PHONE_STATE would, if installed, get
 * null from getDeviceId() on Android 10+. Inside the launcher the caller is the launcher uid
 * (targetSdk 35), so the system throws SecurityException instead. StyleTap.onCreate died on it.
 *
 * The "phone" binder in ServiceManager's cache is wrapped so ITelephony.Stub.asInterface returns
 * a proxy. The proxy changes exactly one case: getDeviceIdWithFeature throwing SecurityException
 * while the current plugin targets SDK 28 or lower and declares READ_PHONE_STATE. That call
 * returns null. Every other method, result and exception passes through unchanged.
 */
final class TelephonyHook {
    private static final String TAG = "zb-launcher";
    private static final String SERVICE = "phone";
    private static final String ITELEPHONY = "com.android.internal.telephony.ITelephony";
    static final String METHOD = "getDeviceIdWithFeature";
    static final int LEGACY_MAX_TARGET_SDK = 28;

    private static volatile String grantedFor;  // plugin package already checked and eligible
    private static volatile String deniedFor;   // plugin package already checked and not eligible

    private TelephonyHook() {}

    /** Installs once per :guest process; records the outcome either way. */
    static void install(Context host, GuestRuntime runtime) {
        String result;
        try {
            result = installOrThrow(host, runtime);
        } catch (Throwable t) {
            result = "failed(" + t + ")";
            Log.w(TAG, "telephony hook not installed", t);
        }
        Diagnostics.note(host, "telephony-hook: " + result);
    }

    private static String installOrThrow(Context host, GuestRuntime runtime) throws Exception {
        Class<?> serviceManager = Class.forName("android.os.ServiceManager");
        IBinder original = (IBinder) Reflect.method(serviceManager, "getService", String.class)
                .invoke(null, SERVICE);
        if (original == null) return "failed(no phone service)";
        if (Proxy.isProxyClass(original.getClass())) return "installed (already)";

        Class<?> iTelephony = Class.forName(ITELEPHONY);
        Class<?> stub = Class.forName(ITELEPHONY + "$Stub");
        Object real = Reflect.method(stub, "asInterface", IBinder.class).invoke(null, original);
        if (real == null) return "failed(asInterface returned null)";
        String descriptor = (String) Reflect.field(stub, "DESCRIPTOR").get(null);

        IBinder[] wrapped = new IBinder[1];
        Object telephony = Proxy.newProxyInstance(iTelephony.getClassLoader(), new Class<?>[] {iTelephony},
                new TelephonyCalls(host, runtime, real, wrapped));
        wrapped[0] = (IBinder) Proxy.newProxyInstance(IBinder.class.getClassLoader(),
                new Class<?>[] {IBinder.class}, (proxy, method, args) -> {
                    if ("queryLocalInterface".equals(method.getName()) && args != null
                            && descriptor.equals(args[0])) {
                        return telephony;
                    }
                    return forward(method, original, args);
                });

        Field cacheField = Reflect.field(serviceManager, "sCache");
        @SuppressWarnings("unchecked")
        Map<String, IBinder> cache = (Map<String, IBinder>) cacheField.get(null);
        if (cache == null) return "failed(ServiceManager.sCache is null)";
        cache.put(SERVICE, wrapped[0]);

        // Confirm TelephonyManager's lookup path now reaches the proxy.
        IBinder check = (IBinder) Reflect.method(serviceManager, "getService", String.class).invoke(null, SERVICE);
        return check == wrapped[0] ? "installed" : "failed(ServiceManager.getService bypasses sCache)";
    }

    private static Object forward(Method method, Object target, Object[] args) throws Throwable {
        try {
            return method.invoke(target, args);
        } catch (InvocationTargetException e) {
            throw e.getCause();  // the original exception, unchanged
        }
    }

    private static final class TelephonyCalls implements InvocationHandler {
        private final Context host;
        private final GuestRuntime runtime;
        private final Object real;
        private final IBinder[] binder;

        TelephonyCalls(Context host, GuestRuntime runtime, Object real, IBinder[] binder) {
            this.host = host;
            this.runtime = runtime;
            this.real = real;
            this.binder = binder;
        }

        @Override
        public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
            String name = method.getName();
            if (method.getDeclaringClass() == Object.class) {
                if ("equals".equals(name)) return proxy == args[0];
                if ("hashCode".equals(name)) return System.identityHashCode(proxy);
                if ("toString".equals(name)) return "ZettaBridge ITelephony proxy";
            }
            if ("asBinder".equals(name) && (args == null || args.length == 0)) return binder[0];
            try {
                return method.invoke(real, args);
            } catch (InvocationTargetException e) {
                Throwable cause = e.getCause();
                if (isCandidate(name, method.getReturnType(), cause)) {
                    LoadedPlugin p = runtime.current();
                    if (p != null && p.appInfo.targetSdkVersion <= LEGACY_MAX_TARGET_SDK
                            && eligible(host, p)) {
                        Diagnostics.note(host, "telephony-call: " + name + " fallback=null targetSdk="
                                + p.appInfo.targetSdkVersion + " plugin=" + p.packageName);
                        return null;
                    }
                }
                throw cause;
            }
        }
    }

    /** targetSdk 28 or lower and READ_PHONE_STATE declared in the plugin manifest. */
    private static boolean eligible(Context host, LoadedPlugin p) {
        if (p.appInfo.targetSdkVersion > LEGACY_MAX_TARGET_SDK) return false;
        if (p.packageName.equals(grantedFor)) return true;
        if (p.packageName.equals(deniedFor)) return false;
        boolean declared = false;
        try {
            PackageInfo info = host.getPackageManager().getPackageArchiveInfo(
                    p.record.apk().getPath(), PackageManager.GET_PERMISSIONS);
            declared = legacyNull(p.appInfo.targetSdkVersion, info == null ? null : info.requestedPermissions);
        } catch (RuntimeException e) {
            Log.w(TAG, "cannot read plugin permissions: " + e);
        }
        if (declared) grantedFor = p.packageName; else deniedFor = p.packageName;
        return declared;
    }

    /** Only getDeviceIdWithFeature returning String and failing with SecurityException. */
    static boolean isCandidate(String method, Class<?> returnType, Throwable failure) {
        return METHOD.equals(method) && returnType == String.class && failure instanceof SecurityException;
    }

    /** The legacy rule: null for targetSdk 28 or lower with READ_PHONE_STATE declared. */
    static boolean legacyNull(int targetSdk, String[] requested) {
        return targetSdk <= LEGACY_MAX_TARGET_SDK && declaresPhoneState(requested);
    }

    static boolean declaresPhoneState(String[] requested) {
        if (requested == null) return false;
        for (String permission : requested) {
            if (Manifest.permission.READ_PHONE_STATE.equals(permission)) return true;
        }
        return false;
    }
}
