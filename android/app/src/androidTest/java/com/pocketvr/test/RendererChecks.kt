// SPDX-License-Identifier: Apache-2.0
package com.pocketvr.test

import android.content.Context
import android.graphics.Bitmap
import android.graphics.PixelFormat
import android.graphics.SurfaceTexture
import android.media.Image
import android.media.ImageReader
import android.media.MediaCodecList
import android.os.SystemClock
import com.pocketvr.NativeTests
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream

object RendererChecks {
    private fun ink(image: Image, eye: Int, lower: Boolean): Int {
        var count = 0
        for (y in (if (lower) 26..30 else 20..24)) for (x in 14..113) {
            val p = pixel(image, x + eye * 128, y)
            if (p[0] in 70..140 && p[1] > 230 && p[2] in 150..220) count++
        }
        return count
    }
    private fun pixel(image: Image, x: Int, y: Int): IntArray {
        return rawPixel(image, x * image.width / 256, y * image.height / 128)
    }
    private fun rawPixel(image: Image, x: Int, y: Int): IntArray {
        val plane = image.planes[0]
        check(plane.pixelStride >= 4)
        val offset = plane.buffer.position() + y * plane.rowStride + x * plane.pixelStride
        return IntArray(3) { plane.buffer.get(offset + it).toInt() and 255 }
    }
    private fun save(context: Context, image: Image, name: String = "renderer-colors.png") {
        val colors = IntArray(image.width * image.height)
        for (y in 0 until image.height) for (x in 0 until image.width) {
            val p = rawPixel(image, x, y)
            colors[y * image.width + x] = (255 shl 24) or (p[0] shl 16) or (p[1] shl 8) or p[2]
        }
        val bitmap = Bitmap.createBitmap(colors, image.width, image.height, Bitmap.Config.ARGB_8888)
        FileOutputStream(File(context.getExternalFilesDir(null), name)).use {
            bitmap.compress(Bitmap.CompressFormat.PNG, 100, it)
        }
        bitmap.recycle()
    }
    @JvmStatic fun run(context: Context) {
        val accessUnit = context.assets.open("fixtures/stereo-256x128.h264").use { it.readBytes() }
        val decoder = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.first {
            !it.isEncoder && it.isHardwareAccelerated && it.supportedTypes.contains("video/avc")
        }.name
        val reader = ImageReader.newInstance(1024, 512, PixelFormat.RGBA_8888, 4)
        val texture = SurfaceTexture(false)
        var handle = 0L
        try {
            handle = NativeTests.rendererOpen(reader.surface, texture, decoder)
            check(handle != 0L)
            NativeTests.rendererCalibration(handle, true)
            var notice = false
            val noticeUntil = SystemClock.elapsedRealtime() + 1500
            while (!notice && SystemClock.elapsedRealtime() < noticeUntil) {
                val image = reader.acquireLatestImage()
                if (image != null) try {
                    notice = (0..1).all { eye -> ink(image, eye, false) > 8 } &&
                        pixel(image, 64, 64).all { it < 10 }
                } finally { image.close() }
                SystemClock.sleep(8)
            }
            check(notice) { "Calibration instruction was not visible without decoded video" }
            NativeTests.rendererCalibration(handle, false)
            val deadline = SystemClock.elapsedRealtime() + 6000
            var matched = false
            var lastPixels = "No output image"
            while (SystemClock.elapsedRealtime() < deadline && !matched) {
                val status = JSONObject(NativeTests.rendererStatus(handle))
                check(status.getJSONObject("renderer").getString("error").isEmpty()) { status.toString() }
                check(status.getJSONObject("decoder").getString("error").isEmpty()) { status.toString() }
                if (status.getJSONObject("decoder").getBoolean("running")) NativeTests.rendererFeed(handle, accessUnit)
                val image = reader.acquireLatestImage()
                if (image != null) try {
                    val red = pixel(image, 32, 32); val green = pixel(image, 32, 96)
                    val yellow = pixel(image, 160, 32); val blue = pixel(image, 160, 96)
                    lastPixels = listOf(red, green, yellow, blue).joinToString { it.joinToString(",") }
                    matched = red[0] > 140 && red[1] < 100 && red[2] < 100 && green[1] > 130 &&
                        green[0] < 100 && green[2] < 100 && yellow[0] > 140 && yellow[1] > 140 && yellow[2] < 100 &&
                        blue[2] > 130 && blue[0] < 100 && blue[1] < 100
                    if (matched) save(context, image)
                } finally { image.close() }
                SystemClock.sleep(20)
            }
            check(matched) { "Stereo texture colours did not match: $lastPixels; ${NativeTests.rendererStatus(handle)}" }
            val timing = JSONObject(NativeTests.rendererStatus(handle)).getJSONObject("decoder")
            check(timing.getInt("decode_samples") in 1..120)
            check(timing.getDouble("decode_p50_ms") in 0.0..50.0)
            check(timing.getDouble("decode_p95_ms") >= timing.getDouble("decode_p50_ms"))
            check(timing.getDouble("decode_p95_ms") < 50.0)
            // Continue consuming display buffers but stop supplying video. The
            // GL cadence must keep re-presenting the last decoded frame.
            val holdUntil = SystemClock.elapsedRealtime() + 180
            while (SystemClock.elapsedRealtime() < holdUntil) { reader.acquireLatestImage()?.close(); SystemClock.sleep(8) }
            val status = JSONObject(NativeTests.rendererStatus(handle))
            check(status.getJSONObject("renderer").getLong("repeated") >= 2) { "Renderer stopped without new video: $status" }
            NativeTests.rendererGrid(handle, true)
            var grid = false
            val gridUntil = SystemClock.elapsedRealtime() + 1000
            while (!grid && SystemClock.elapsedRealtime() < gridUntil) {
                val image = reader.acquireLatestImage()
                if (image != null) try {
                    val center = pixel(image, 64, 64)
                    grid = center[0] > 180 && center[1] in 30..130 && center[2] < 100
                } finally { image.close() }
                SystemClock.sleep(8)
            }
            check(grid) { "Calibration grid did not render" }
            NativeTests.rendererGrid(handle, false)
            var warped = false
            val warpUntil = SystemClock.elapsedRealtime() + 1000
            while (!warped && SystemClock.elapsedRealtime() < warpUntil) {
                NativeTests.rendererPose(handle, 0.35)
                val image = reader.acquireLatestImage()
                if (image != null) try {
                    val border = pixel(image, 8, 32)
                    val middle = pixel(image, 80, 32)
                    // Even beyond the guard, extend the source edge instead of
                    // introducing a gyro-following black strip in either eye.
                    warped = border[0] > 140 && border[1] < 100 && middle[0] > 140 && middle[1] < 100
                    if (warped) save(context, image)
                } finally { image.close() }
                SystemClock.sleep(8)
            }
            check(warped) { "Late rotational reprojection introduced a moving black border" }
            // Verify the actual GPU output, including matching text in both
            // eyes, full/minimal row visibility, disabling, and PC inheritance.
            NativeTests.rendererGrid(handle, true)
            for (mode in listOf(2, 1, 0, -1)) {
                NativeTests.rendererStats(handle, mode)
                var visible = false
                val until = SystemClock.elapsedRealtime() + 1500
                while (!visible && SystemClock.elapsedRealtime() < until) {
                    val image = reader.acquireLatestImage()
                    if (image != null) try {
                        visible = (0..1).all { eye ->
                            val top = ink(image, eye, false)
                            val bottom = ink(image, eye, true)
                            if (mode == 2) top > 8 && bottom > 8
                            else if (mode == 1) top > 8 && bottom == 0
                            else top == 0 && bottom == 0
                        }
                        if (visible && mode == 2) save(context, image, "renderer-stats.png")
                    } finally { image.close() }
                    SystemClock.sleep(8)
                }
                check(visible) { "Stereo stats visibility failed for mode $mode" }
            }
            var rejected = false
            NativeTests.rendererGrid(handle, false)
            NativeTests.rendererStats(handle, 0)
            NativeTests.rendererCooling(handle, true)
            var cooling = false
            val coolingUntil = SystemClock.elapsedRealtime() + 1500
            while (!cooling && SystemClock.elapsedRealtime() < coolingUntil) {
                val image = reader.acquireLatestImage()
                if (image != null) try {
                    cooling = (0..1).all { eye -> ink(image, eye, false) > 8 && pixel(image, 64 + eye * 128, 64).all { it < 10 } }
                    if (cooling) save(context, image, "renderer-cooling.png")
                } finally { image.close() }
                SystemClock.sleep(8)
            }
            check(cooling) { "Cooling warning was hidden when normal stats were off" }
            NativeTests.rendererCooling(handle, false)
            try { NativeTests.rendererStats(handle, 3) } catch (_: IllegalStateException) { rejected = true }
            check(rejected) { "Invalid stats mode accepted" }
            val metrics = JSONObject(NativeTests.rendererStatus(handle)).getJSONObject("renderer")
            check(metrics.getDouble("display_fps").isFinite() && metrics.getDouble("display_fps") > 0)
            check(metrics.getDouble("video_fps").isFinite() && metrics.getDouble("video_fps") >= 0)
        } finally {
            // Abandon the output consumer before joining GL, including on a
            // test failure, so a full ImageReader cannot block eglSwapBuffers.
            reader.close()
            if (handle != 0L) NativeTests.rendererClose(handle)
            texture.release()
        }
    }
}
