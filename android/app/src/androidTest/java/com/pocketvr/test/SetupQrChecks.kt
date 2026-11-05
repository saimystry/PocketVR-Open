// SPDX-License-Identifier: Apache-2.0
package com.pocketvr.test

import android.app.Instrumentation
import android.util.Base64
import com.google.zxing.BinaryBitmap
import com.google.zxing.RGBLuminanceSource
import com.google.zxing.PlanarYUVLuminanceSource
import com.google.zxing.common.HybridBinarizer
import com.google.zxing.qrcode.QRCodeReader
import com.pocketvr.SetupQr
import com.pocketvr.NativeBridge
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

object SetupQrChecks {
    private fun rejected(action: () -> Unit) {
        var failed = false
        try { action() } catch (_: Exception) { failed = true }
        check(failed) { "Malformed setup QR was accepted" }
    }
    @JvmStatic fun run(instrumentation: Instrumentation) {
        val valid = "pocketvr://connect?v=1&ip=192.168.42.7&port=12345"
        check(SetupQr.connection(valid) == SetupQr.Connection("192.168.42.7", 12345))
        for (bad in listOf(valid + "&port=1", valid + "&key=trust", valid.replace("v=1", "v=2"),
            valid.replace("192.168.42.7", "8.8.8.8"), valid.replace("192.168.42.7", "127.0.0.1"),
            valid.replace("192.168.42.7", "192.168.042.7"), valid.replace("12345", "65536"), valid + "#ignored"))
            rejected { SetupQr.connection(bad) }
        rejected { SetupQr.connection(valid + "x".repeat(256)) }
        rejected { SetupQr.connection(valid + "&%69p=10.0.0.1") }
        // Fixture exported by the actual desktop C++ generator, decoded by unrelated Java ZXing.
        val fixture = instrumentation.context.assets.open("desktop-connection-qr.txt").bufferedReader().readLines()
        val size = fixture[1].toInt(); val scale = 6; val side = (size + 8) * scale
        val pixels = IntArray(side * side) { -1 }
        for (y in 0 until size) for (x in 0 until size) if (fixture[y + 2][x] == '1')
            for (dy in 0 until scale) for (dx in 0 until scale)
                pixels[((y + 4) * scale + dy) * side + (x + 4) * scale + dx] = -0x1000000
        val reader = QRCodeReader()
        val decoded = reader.decode(BinaryBitmap(HybridBinarizer(RGBLuminanceSource(side, side, pixels)))).text
        check(decoded == fixture[0] && decoded == valid)
        check(SetupQr.connection(decoded).port == 12345)
        val nv21 = ByteArray(side * side * 3 / 2) { 128.toByte() }
        for (i in pixels.indices) nv21[i] = if (pixels[i] == -1) 255.toByte() else 0
        check(QRCodeReader().decode(BinaryBitmap(HybridBinarizer(
            PlanarYUVLuminanceSource(nv21, side, side, 0, 0, side, side, false)))).text == valid)

        fun profile(alignment: Int = 0, coefficients: List<Float> = listOf(0.34f, 0.55f), packed: Boolean = true): ByteArray {
            val output = ByteArrayOutputStream()
            fun number(value: Float) { output.write(ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putFloat(value).array()) }
            output.write(10); output.write(6); output.write("Vendor".toByteArray())
            output.write(18); output.write(5); output.write("Model".toByteArray())
            output.write(29); number(0.039f); output.write(37); number(0.064f)
            if (packed) { output.write(42); output.write(16) }
            for (v in listOf(40f, 45f, 42f, 43f)) { if (!packed) output.write(45); number(v) }
            output.write(53); number(0.035f)
            output.write(58); output.write(coefficients.size * 4); coefficients.forEach { number(it) }
            output.write(88); output.write(alignment)
            output.write(96); output.write(3) // Advisory primary button is skipped.
            return output.toByteArray()
        }
        fun url(bytes: ByteArray) = "https://google.com/cardboard/cfg?p=" + Base64.encodeToString(bytes, Base64.URL_SAFE or Base64.NO_WRAP or Base64.NO_PADDING)
        val base = JSONObject(NativeBridge.viewerCatalog()).getJSONObject("values").put("ipd_mm", 68.0)
        val viewer = SetupQr.viewer(url(profile()))
        check(viewer.name == "Vendor · Model")
        val calibration = viewer.calibration(base, 140.0, 60.0, 3.0)
        check(kotlin.math.abs(calibration.getDouble("vertical_offset_m") - 0.002) < 1e-7)
        check(calibration.getDouble("ipd_mm") == 68.0 && calibration.getDouble("fov_top_deg") == 43.0)
        val saved = JSONObject(NativeBridge.validateViewer(calibration.toString()))
        check(saved.getDouble("ipd_mm") == 68.0)
        val top = SetupQr.viewer(url(profile(2))).calibration(base, 140.0, 60.0, 3.0)
        check(kotlin.math.abs(top.getDouble("vertical_offset_m") + calibration.getDouble("vertical_offset_m")) < 1e-8)
        check(SetupQr.viewer(url(profile(1, packed = false))).calibration(base, 140.0, 60.0, 3.0).getDouble("vertical_offset_m") == 0.0)
        rejected { SetupQr.viewer(url(profile(coefficients = listOf(0.1f, 0.2f, 0.3f)))) }
        rejected { SetupQr.viewer(url(profile(coefficients = listOf(Float.NaN)))) }
        rejected { SetupQr.viewer(url(profile(coefficients = listOf(-0.1f)))) }
        rejected { SetupQr.viewer(url(profile(alignment = 3))) }
        rejected { SetupQr.viewer("https://evil.example/cardboard/cfg?p=AA") }
        rejected { SetupQr.viewer("https://goo.gl/example") }
        rejected { SetupQr.viewer("x".repeat(4097)) }
        rejected { SetupQr.viewer(url(profile()) + "&p=AA") }
        rejected { SetupQr.viewer(url(profile()) + "&%70=AA") }
        val bytes = profile()
        for (length in 0 until 25) rejected { SetupQr.viewer(url(bytes.copyOf(length))) }
        rejected { NativeBridge.validateViewer(viewer.calibration(base, 60.0, 40.0, 3.0).toString()) }
        // Legacy saved geometry migrates without altering measured fields.
        for (key in listOf("vertical_offset_m", "fov_left_deg", "fov_right_deg", "fov_bottom_deg", "fov_top_deg")) base.remove(key)
        check(JSONObject(NativeBridge.validateViewer(base.toString())).getDouble("ipd_mm") == 68.0)
    }
}
