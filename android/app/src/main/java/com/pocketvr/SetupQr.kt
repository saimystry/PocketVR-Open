// SPDX-License-Identifier: Apache-2.0
package com.pocketvr

import android.net.Uri
import android.util.Base64
import org.json.JSONObject
import java.nio.ByteBuffer
import java.nio.ByteOrder

/** Setup-only, bounded offline formats. A connection code never grants trust. */
object SetupQr {
    data class Connection(val ip: String, val port: Int)
    fun connection(text: String): Connection {
        require(text.length in 1..256) { "Connection code is too long" }
        val uri = Uri.parse(text)
        require(uri.scheme == "pocketvr" && uri.host == "connect" && uri.path.isNullOrEmpty() &&
            uri.userInfo == null && uri.port == -1 && uri.fragment == null &&
            uri.encodedQuery?.split('&')?.size == 3 &&
            uri.queryParameterNames == setOf("v", "ip", "port") &&
            uri.queryParameterNames.all { uri.getQueryParameters(it).size == 1 } &&
            uri.getQueryParameter("v") == "1") { "Not a PocketVR v1 connection code" }
        val ip = uri.getQueryParameter("ip") ?: ""
        val parts = ip.split('.')
        require(parts.size == 4 && parts.all { it.matches(Regex("0|[1-9][0-9]{0,2}")) && it.toInt() <= 255 }) { "Invalid PC address" }
        val p = parts.map { it.toInt() }
        require(p[0] == 10 || (p[0] == 172 && p[1] in 16..31) ||
            (p[0] == 192 && p[1] == 168)) { "Use a private LAN address; USB setup is separate" }
        val port = uri.getQueryParameter("port")?.toIntOrNull()
        require(port != null && port in 1..65535) { "Invalid control port" }
        return Connection(ip, port)
    }

    data class Viewer(val name: String, val distance: Double, val spacing: Double,
                      val tray: Double, val alignment: Int, val fov: List<Double>, val k: List<Double>) {
        fun calibration(base: JSONObject, widthMm: Double, heightMm: Double, borderMm: Double): JSONObject {
            require(widthMm.isFinite() && heightMm.isFinite() && borderMm.isFinite() && borderMm in 0.0..20.0) { "Check screen and bezel measurements" }
            val h = heightMm / 1000
            val bottom = when (alignment) { 0 -> tray - borderMm / 1000; 1 -> h / 2; else -> h - tray + borderMm / 1000 }
            require(bottom > 0 && bottom < h) { "Lens center must fall inside the active screen" }
            return JSONObject(base.toString()).put("screen_width_m", widthMm / 1000).put("screen_height_m", h)
                .put("screen_to_lens_m", distance).put("inter_lens_distance_m", spacing)
                .put("vertical_offset_m", bottom - h / 2).put("fov_scale", 1.0)
                .put("fov_left_deg", fov[0]).put("fov_right_deg", fov[1])
                .put("fov_bottom_deg", fov[2]).put("fov_top_deg", fov[3])
                .put("k1", k.getOrElse(0) { 0.0 }).put("k2", k.getOrElse(1) { 0.0 })
        }
    }

    /** DeviceParams wire format pinned in third_party/cardboard_device.proto.
     * No URLs are fetched. Unsupported polynomials fail instead of being truncated. */
    fun viewer(text: String): Viewer {
        require(text.length in 1..4096) { "Viewer code is too long" }
        val uri = Uri.parse(text)
        require(uri.scheme in setOf("http", "https") && uri.host in setOf("google.com", "www.google.com") &&
            uri.path == "/cardboard/cfg" && uri.userInfo == null && uri.port == -1 && uri.fragment == null &&
            uri.encodedQuery?.split('&')?.size == 1 &&
            uri.queryParameterNames == setOf("p") && uri.getQueryParameters("p").size == 1) {
            "Use a full Cardboard configuration QR (google.com/cardboard/cfg?p=…). Short links are not opened."
        }
        val encoded = uri.getQueryParameter("p") ?: ""
        require(encoded.matches(Regex("[A-Za-z0-9_-]{1,3500}={0,2}"))) { "Invalid Cardboard data" }
        val data = Base64.decode(encoded, Base64.URL_SAFE or Base64.NO_WRAP)
        require(data.size in 1..2048) { "Invalid Cardboard data size" }
        var offset = 0
        fun varint(): Int {
            var value = 0L
            for (i in 0..4) {
                require(offset < data.size) { "Truncated viewer data" }
                val b = data[offset++].toInt() and 255
                value = value or ((b and 127).toLong() shl (i * 7))
                if (b < 128) { require(value <= Int.MAX_VALUE) { "Viewer number is too large" }; return value.toInt() }
            }
            throw IllegalArgumentException("Malformed viewer number")
        }
        fun bytes(length: Int): ByteArray {
            require(length >= 0 && length <= data.size - offset) { "Truncated viewer field" }
            return data.copyOfRange(offset, offset + length).also { offset += length }
        }
        fun number(): Double = ByteBuffer.wrap(bytes(4)).order(ByteOrder.LITTLE_ENDIAN).float.toDouble().also {
            require(it.isFinite()) { "Nonfinite viewer measurement" }
        }
        val singular = mutableSetOf<Int>()
        var vendor = ""; var model = ""; var distance = Double.NaN; var spacing = Double.NaN
        var tray = Double.NaN; var alignment = 0
        val fov = mutableListOf<Double>(); val k = mutableListOf<Double>()
        while (offset < data.size) {
            val tag = varint(); val field = tag ushr 3; val wire = tag and 7
            require(field > 0) { "Invalid viewer field" }
            if (field in setOf(1, 2, 3, 4, 6, 11)) require(singular.add(field)) { "Duplicate viewer field" }
            when (field) {
                1, 2 -> {
                    require(wire == 2); val value = bytes(varint()).toString(Charsets.UTF_8)
                    require(value.length <= 96 && value.none { it.isISOControl() }) { "Invalid viewer name" }
                    if (field == 1) vendor = value else model = value
                }
                3, 4, 6 -> {
                    require(wire == 5); val n = number()
                    when (field) { 3 -> distance = n; 4 -> spacing = n; else -> tray = n }
                }
                5, 7 -> {
                    val target = if (field == 5) fov else k
                    if (wire == 5) target.add(number())
                    else {
                        require(wire == 2); val size = varint()
                        require(size % 4 == 0 && size <= 64 && size <= data.size - offset)
                        repeat(size / 4) { target.add(number()) }
                    }
                    require(target.size <= (if (field == 5) 4 else 16)) { "Too many viewer coefficients" }
                }
                11 -> { require(wire == 0); alignment = varint(); require(alignment in 0..2) }
                else -> when (wire) {
                    0 -> varint()
                    1 -> bytes(8)
                    2 -> bytes(varint())
                    5 -> bytes(4)
                    else -> throw IllegalArgumentException("Unsupported viewer wire type")
                }
            }
        }
        require(distance in 0.02..0.08 && spacing in 0.04..0.09 &&
            (alignment == 1 || tray in 0.0..0.12) && fov.size == 4 && fov.all { it in 1.0..89.0 }) { "Viewer measurements are incomplete or outside supported limits" }
        require(k.size <= 2 && k.all { it in 0.0..2.0 }) { "This viewer's distortion polynomial is unsupported; use manual calibration" }
        return Viewer(listOf(vendor, model).filter { it.isNotEmpty() }.joinToString(" · ").ifEmpty { "Cardboard viewer" },
            distance, spacing, tray, alignment, fov, k)
    }
}
