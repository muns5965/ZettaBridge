package com.zettabridge.launcher;

import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.util.DisplayMetrics;
import android.util.Log;

import java.lang.reflect.Method;

/**
 * Plugin resources that answer name lookups made with the launcher's package name.
 *
 * Plugin contexts keep the launcher's identity, so getPackageName() returns the launcher package.
 * Old apps look up their own views by name with that value, for example StyleTap:
 * getResources().getIdentifier("dummyEditField", "id", getPackageName()). The resource table only
 * knows the plugin package, so the lookup returned 0 and findViewById(0) returned null.
 *
 * Only getIdentifier is changed, and only when the request names the launcher package. A null or
 * empty package, or any other package, is passed through untouched.
 */
final class PluginResources extends Resources {
    private static final String TAG = "zb-launcher";

    private final String hostPackage;
    private final String pluginPackage;
    private final Resources source;

    /** Wraps resources already loaded for the plugin, sharing their implementation. */
    static PluginResources wrap(Resources source, String hostPackage, String pluginPackage) {
        PluginResources r = new PluginResources(source.getAssets(), source.getDisplayMetrics(),
                source.getConfiguration(), hostPackage, pluginPackage, source);
        r.syncImpl();
        return r;
    }

    /** New resources over the plugin assets with an explicit configuration (derived contexts). */
    @SuppressWarnings("deprecation")
    PluginResources(AssetManager assets, DisplayMetrics metrics, Configuration config,
                    String hostPackage, String pluginPackage) {
        this(assets, metrics, config, hostPackage, pluginPackage, null);
    }

    @SuppressWarnings("deprecation")
    private PluginResources(AssetManager assets, DisplayMetrics metrics, Configuration config,
                            String hostPackage, String pluginPackage, Resources source) {
        super(assets, metrics, config);
        this.hostPackage = hostPackage;
        this.pluginPackage = pluginPackage;
        this.source = source;
    }

    /**
     * Shares the ResourcesImpl of the resources this wraps, so compatibility info, loaders and
     * configuration stay exactly what the framework built, and themes behave the same. The
     * framework swaps the source's impl on a configuration change; calling this again (before
     * each plugin activity is created) picks up the new one. Without the hidden API the
     * constructor-built impl stays, which is how derived contexts already worked.
     */
    void syncImpl() {
        if (source == null) return;
        try {
            Method getImpl = Reflect.method(Resources.class, "getImpl");
            Object impl = getImpl.invoke(source);
            if (impl == null || impl == getImpl.invoke(this)) return;
            Reflect.method(Resources.class, "setImpl", implClass()).invoke(this, impl);
        } catch (ReflectiveOperationException | RuntimeException e) {
            Log.w(TAG, "cannot share plugin ResourcesImpl, using a copy: " + e);
        }
    }

    private static Class<?> implClass() throws ClassNotFoundException {
        return Class.forName("android.content.res.ResourcesImpl");
    }

    @Override
    public int getIdentifier(String name, String defType, String defPackage) {
        return super.getIdentifier(mapName(hostPackage, pluginPackage, name), defType,
                mapPackage(hostPackage, pluginPackage, defPackage));
    }

    /** "launcher.pkg" -> "plugin.pkg"; anything else, including null and "", is unchanged. */
    static String mapPackage(String hostPackage, String pluginPackage, String pkg) {
        return hostPackage.equals(pkg) ? pluginPackage : pkg;
    }

    /** "[@]launcher.pkg:type/entry" -> "[@]plugin.pkg:type/entry"; other names are unchanged. */
    static String mapName(String hostPackage, String pluginPackage, String name) {
        if (name == null) return null;
        int start = name.startsWith("@") ? 1 : 0;
        if (!name.startsWith(hostPackage + ":", start)) return name;
        return name.substring(0, start) + pluginPackage + name.substring(start + hostPackage.length());
    }
}
