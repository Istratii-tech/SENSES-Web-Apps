package org.arcwasm.splittest.feature;

/** In split_feature.apk's classes.dex: loaded by the base's MainActivity. */
public final class Feature {
    private Feature() {}

    public static String hello() {
        return "feature-" + Feature.class.getName().length();
    }
}
