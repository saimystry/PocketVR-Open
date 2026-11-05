// SPDX-License-Identifier: Apache-2.0
package com.pocketvr;

public final class NativeTests {
    private NativeTests() {}
    static { System.loadLibrary("pocketvr_native"); }
    public static native int run();
    public static native double[] sensorVector(double x, double y, double z, int rotation);
    public static native long rendererOpen(android.view.Surface surface, android.graphics.SurfaceTexture texture, String codec);
    public static native boolean rendererFeed(long handle, byte[] bytes);
    public static native String rendererStatus(long handle);
    public static native void rendererClose(long handle);
    public static native void rendererPose(long handle, double yaw);
    public static native void rendererGrid(long handle, boolean enabled);
    public static native void rendererStats(long handle, int mode);
    public static native void rendererCalibration(long handle, boolean enabled);
    public static native void rendererCooling(long handle, boolean enabled);
}
