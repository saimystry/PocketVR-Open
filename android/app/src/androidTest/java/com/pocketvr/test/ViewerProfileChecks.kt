// SPDX-License-Identifier: Apache-2.0
package com.pocketvr.test

import android.content.Context
import android.content.Intent
import android.app.AlertDialog
import android.app.Instrumentation
import android.view.View
import android.widget.SeekBar
import android.graphics.Bitmap
import com.pocketvr.MainActivity
import com.pocketvr.NativeBridge
import com.pocketvr.ViewerProfiles
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream

object ViewerProfileChecks {
    @JvmStatic fun run(instrumentation: Instrumentation) {
        val context = instrumentation.targetContext
        val name = "viewer-calibration-test-${System.nanoTime()}"
        val preferences = context.getSharedPreferences(name, Context.MODE_PRIVATE)
        try {
            val store = ViewerProfiles(preferences)
            check(store.saved() == null)
            val values = JSONObject(NativeBridge.viewerCatalog()).getJSONObject("values")
                .put("screen_to_lens_m", 0.045).put("inter_lens_distance_m", 0.065).put("ipd_mm", 68.0)
            val saved = store.save(values.toString())
            val reloaded = ViewerProfiles(context.getSharedPreferences(name, Context.MODE_PRIVATE))
            check(reloaded.saved() == saved) { "Calibration did not persist" }
            check(JSONObject(saved).getDouble("ipd_mm") == 68.0)
            val bad = JSONObject(saved).put("screen_to_lens_m", 0.001)
            var rejected = false
            try { store.save(bad.toString()) } catch (_: IllegalStateException) { rejected = true }
            check(rejected && reloaded.saved() == saved) { "Invalid calibration replaced saved values" }
            val handle = NativeBridge.create(ByteArray(32) { (it + 1).toByte() }, "{\"codecs\":[]}")
            try {
                NativeBridge.viewer(handle, saved)
                check(JSONObject(NativeBridge.status(handle)).getString("viewer_sync") == "Saved on phone; waiting for PC")
                NativeBridge.stop(handle)
                check(JSONObject(NativeBridge.status(handle)).getString("viewer_sync").startsWith("Saved on phone"))
            } finally { NativeBridge.destroy(handle) }
        } finally { context.deleteSharedPreferences(name) }
        val activity = instrumentation.startActivitySync(Intent(context, MainActivity::class.java)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
        fun main(action: () -> Unit) {
            var failure: Throwable? = null
            instrumentation.runOnMainSync { try { action() } catch (error: Throwable) { failure = error } }
            failure?.let { throw IllegalStateException("Viewer UI check failed: ${it.message}", it) }
        }
        try {
            val existing = context.getSharedPreferences("viewer", Context.MODE_PRIVATE).getString("manual_viewer", null)
            lateinit var dialog: AlertDialog
            main {
                val button = activity.window.decorView.findViewWithTag<View>("viewer_calibration")
                check(button != null && button.performClick()) { "Viewer calibration button is missing" }
                dialog = checkNotNull(activity.viewerDialog) { "Viewer calibration dialog did not open" }
                check(dialog.isShowing)
            }
            instrumentation.waitForIdleSync()
            main {
                val window = dialog.window!!.decorView
                val distance = window.findViewWithTag<SeekBar>("viewer_screen_to_lens_m")
                check(distance != null && distance.max == 600) { "Lens distance is not a 20-80 mm slider" }
                check(distance.progress == 190) { "Lens distance slider is not at the 39 mm default" }
                check(window.findViewWithTag<SeekBar>("viewer_k1") != null) { "Advanced lens slider is missing" }
                distance.progress = 0
                check(context.getSharedPreferences("viewer", Context.MODE_PRIVATE).getString("manual_viewer", null) == existing)
                window.findViewWithTag<View>("viewer_generic").performClick()
                check(distance.progress == 190) { "Reset did not restore the lens distance slider" }
            }
            instrumentation.waitForIdleSync()
            val screenshot = instrumentation.uiAutomation.takeScreenshot()
            if (screenshot != null) {
                FileOutputStream(File(context.getExternalFilesDir(null), "viewer-calibration-check.png")).use {
                    screenshot.compress(Bitmap.CompressFormat.PNG, 100, it)
                }
                screenshot.recycle()
            }
            main {
                check(dialog.isShowing) { "Calibration dialog closed before Cancel" }
                dialog.getButton(AlertDialog.BUTTON_NEGATIVE).performClick()
            }
            instrumentation.waitForIdleSync()
            main { check(activity.viewerDialog == null) { "Cancel did not dismiss calibration" } }
        } finally { instrumentation.runOnMainSync { activity.finish() } }
    }
}
