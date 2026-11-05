// SPDX-License-Identifier: Apache-2.0
package com.pocketvr.test;

import android.app.Activity;
import android.app.Instrumentation;
import android.os.Bundle;

public final class NativeInstrumentation extends Instrumentation {
    private Bundle arguments;
    @Override public void onCreate(Bundle arguments) { super.onCreate(arguments); this.arguments = arguments; start(); }
    @Override public void onStart() {
        Bundle result = new Bundle();
        try {
            if (arguments != null && arguments.containsKey("live_server")) {
                result.putString("live_session", LiveSessionChecks.run(this, arguments.getString("live_server"), "true".equals(arguments.getString("live_usb")), "true".equals(arguments.getString("live_auto")), arguments.getString("live_expected_route", "")));
                finish(Activity.RESULT_OK, result);
                return;
            }
            SetupQrChecks.run(this);
            TransportSelectionChecks.run();
            result.putInt("transport_selection_checks", 1);
            result.putInt("qr_checks", 1);
            if (arguments != null && "true".equals(arguments.getString("qr_only"))) {
                result.putInt("native_failures", 0);
                finish(Activity.RESULT_OK, result);
                return;
            }
            Class<?> tests = getTargetContext().getClassLoader().loadClass("com.pocketvr.NativeTests");
            if (!com.pocketvr.BuildConfig.NATIVE_TESTS) {
                result.putString("stream", "Native/GPU checks skipped: rebuild with -PnativeTests=true\n");
                result.putInt("native_skipped", 1);
                finish(Activity.RESULT_OK, result);
                return;
            }
            int failures = (Integer) tests.getMethod("run").invoke(null);
            SensorDirectionChecks.run();
            result.putInt("sensor_direction_checks", 1);
            SecureStoreChecks.run(getTargetContext());
            result.putInt("secure_store_checks", 1);
            NativeHandleChecks.run();
            result.putInt("native_handle_checks", 1);
            RendererChecks.run(getTargetContext());
            result.putInt("renderer_checks", 1);
            ViewerProfileChecks.run(this);
            result.putInt("viewer_checks", 1);
            result.putInt("native_failures", failures);
            result.putString("stream", failures == 0 ? "PocketVR native tests passed\n" : "PocketVR native tests failed\n");
            finish(failures == 0 ? Activity.RESULT_OK : Activity.RESULT_CANCELED, result);
        } catch (Exception | LinkageError error) {
            result.putInt("native_failures", -1);
            result.putString("stream", "Native test launch failed: " + android.util.Log.getStackTraceString(error) + "\n");
            finish(Activity.RESULT_CANCELED, result);
        }
    }
}
