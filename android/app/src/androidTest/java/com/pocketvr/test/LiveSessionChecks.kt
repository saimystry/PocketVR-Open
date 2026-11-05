// SPDX-License-Identifier: Apache-2.0
package com.pocketvr.test

import android.app.Instrumentation
import android.content.Intent
import android.os.Bundle
import android.os.SystemClock
import android.view.View
import android.widget.EditText
import android.widget.Switch
import com.pocketvr.MainActivity
import com.pocketvr.NativeBridge
import org.json.JSONObject
import android.graphics.Bitmap
import java.io.File
import java.io.FileOutputStream

/** Opt-in hardware check. Never run as part of ordinary native/CI tests. */
object LiveSessionChecks {
    @JvmStatic fun run(instrumentation: Instrumentation, ip: String, usb: Boolean, automatic: Boolean, expectedRoute: String): String {
        val activity = instrumentation.startActivitySync(Intent(instrumentation.targetContext, MainActivity::class.java)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
        fun main(action: () -> Unit) {
            var failure: Throwable? = null
            instrumentation.runOnMainSync { try { action() } catch (error: Throwable) { failure = error } }
            failure?.let { throw IllegalStateException("Live UI check failed", it) }
        }
        val field = MainActivity::class.java.getDeclaredField("nativeHandle").apply { isAccessible = true }
        val handle = field.getLong(activity)
        check(handle != 0L)
        try {
            main {
                activity.window.decorView.findViewWithTag<Switch>("automatic_transport").isChecked = false
                activity.window.decorView.findViewWithTag<Switch>("usb_adb").isChecked = usb
                activity.window.decorView.findViewWithTag<EditText>("pc_address").setText(ip)
                activity.window.decorView.findViewWithTag<EditText>("pc_port").setText("9943")
                if (automatic) activity.window.decorView.findViewWithTag<Switch>("automatic_transport").isChecked = true
                activity.window.decorView.findViewWithTag<View>("connect").performClick()
            }
            val deadline = SystemClock.elapsedRealtime() + 90000
            var last = JSONObject()
            var connectedAt = 0L
            var enteredHeadset = false
            var headsetAt = 0L
            var stableAt = 0L
            var firstDecoded = 0L
            var firstLatched = 0L
            var previousDecoded = 0L
            var previousPoses = 0L
            var previewConnections = 0L
            var previewDecoded = 0L
            var previewPoses = 0L
            var exitedAt = 0L
            var reentered = false
            while (SystemClock.elapsedRealtime() < deadline) {
                last = JSONObject(NativeBridge.status(handle))
                if (last.optBoolean("connected") && connectedAt == 0L) connectedAt = SystemClock.elapsedRealtime()
                if (!enteredHeadset && connectedAt != 0L && SystemClock.elapsedRealtime() - connectedAt > 3000 &&
                    last.optJSONObject("sensors")?.optBoolean("calibrating") == false &&
                    (last.optJSONObject("decoder")?.optLong("decoded") ?: 0) > 10) {
                    previewConnections = last.getLong("connections")
                    previewDecoded = last.getJSONObject("decoder").getLong("decoded")
                    previewPoses = last.getLong("poses_sent")
                    main { activity.window.decorView.findViewWithTag<View>("headset_view").performClick() }
                    enteredHeadset = true
                    headsetAt = SystemClock.elapsedRealtime()
                }
                val decoded = last.optJSONObject("decoder")?.optLong("decoded") ?: 0
                val poses = last.optLong("poses_sent")
                if (enteredHeadset) {
                    check(last.optBoolean("connected")) { "Headset transition disconnected the session" }
                    check(last.getLong("connections") == previewConnections) { "Headset transition authenticated a new session" }
                    check(decoded >= previewDecoded && poses >= previewPoses) { "Headset transition restarted video or tracking counters" }
                }
                if (enteredHeadset && stableAt == 0L && SystemClock.elapsedRealtime() - headsetAt > 1500 &&
                    last.optBoolean("connected") && last.optBoolean("clock_ready") && decoded > 10 && poses > 100) {
                    stableAt = SystemClock.elapsedRealtime()
                    firstDecoded = decoded
                    firstLatched = last.getJSONObject("renderer").getLong("latched")
                    previousDecoded = decoded
                    previousPoses = poses
                }
                if (stableAt != 0L) {
                    check(last.optBoolean("connected")) { "Fullscreen session disconnected: $last" }
                    check(decoded >= previousDecoded && poses >= previousPoses) { "Fullscreen session restarted: $last" }
                    check(last.getJSONObject("stream").getString("transport") == if (usb) "tcp" else "udp") {
                        "Unexpected media transport: $last"
                    }
                    previousDecoded = decoded
                    previousPoses = poses
                }
                if (stableAt != 0L && exitedAt == 0L && SystemClock.elapsedRealtime() - stableAt > 5000) {
                    main { activity.onBackPressed() }
                    exitedAt = SystemClock.elapsedRealtime()
                }
                if (exitedAt != 0L && !reentered && SystemClock.elapsedRealtime() - exitedAt > 1500) {
                    main { activity.window.decorView.findViewWithTag<View>("headset_view").performClick() }
                    reentered = true
                }
                if (stableAt != 0L && SystemClock.elapsedRealtime() - stableAt > 20000) {
                    val route = MainActivity::class.java.getDeclaredField("routeLabel").apply { isAccessible = true }.get(activity) as String
                    if (expectedRoute.isNotEmpty()) check(route == expectedRoute) { "Expected $expectedRoute; selected $route" }
                    last.put("selected_route", route)
                    last.put("headset_transitions_without_reconnect", reentered)
                    check(reentered) { "Did not complete headset enter/exit/reenter check" }
                    last.put("selected_ip", MainActivity::class.java.getDeclaredField("connectedIp").apply { isAccessible = true }.get(activity))
                    check(last.optBoolean("connected")) { "Live session disconnected: $last" }
                    check((last.optJSONObject("decoder")?.optLong("decoded") ?: 0) > 10) { "No live decoded video: $last" }
                    check(decoded - firstDecoded > 300) { "Insufficient continuous video delivery: $last" }
                    check((last.optJSONObject("renderer")?.optLong("latched") ?: 0) > 10) { "No live rendered video: $last" }
                    check(last.getJSONObject("renderer").getLong("latched") - firstLatched > 300) { "Rendering stopped advancing after the transitions" }
                    check(last.getJSONObject("renderer").getBoolean("running") && last.getJSONObject("renderer").optString("error").isEmpty()) { "Renderer failed after the transitions" }
                    check(last.optLong("poses_sent") > 100) { "No live tracking delivery: $last" }
                    check(!last.getJSONObject("sensors").getBoolean("calibrating")) { "Fullscreen lost gyro calibration" }
                    instrumentation.uiAutomation.takeScreenshot()?.let { image ->
                        FileOutputStream(File(activity.getExternalFilesDir(null), "live-headset.png")).use {
                            image.compress(Bitmap.CompressFormat.PNG, 100, it)
                        }
                        image.recycle()
                    }
                    return last.toString()
                }
                SystemClock.sleep(100)
            }
            error("Live session timed out: $last")
        } finally { main { activity.finish() } }
    }
}
