// SPDX-License-Identifier: Apache-2.0
package com.pocketvr

object NativeBridge {
    init { System.loadLibrary("pocketvr_native") }
    external fun create(seed: ByteArray, capabilities: String): Long
    external fun destroy(handle: Long)
    external fun connect(handle: Long, ip: String, port: Int, pinnedIdentity: String, allowPairing: Boolean, tcpMedia: Boolean, wiredLink: Boolean)
    external fun stop(handle: Long)
    external fun discover(handle: Long, enabled: Boolean)
    external fun status(handle: Long): String
    external fun rotation(handle: Long, rotation: Int)
    external fun displayRate(handle: Long, hz: Double)
    external fun thermal(handle: Long, status: Int)
    external fun recenter(handle: Long, calibration: Boolean)
    external fun surface(handle: Long, surface: android.view.Surface?, texture: android.graphics.SurfaceTexture?, singleBuffer: Boolean)
    external fun grid(handle: Long, enabled: Boolean)
    external fun viewMode(handle: Long, mode: Int)
    external fun viewSize(handle: Long, size: Double)
    external fun setting(handle: Long, key: String, value: String)
    external fun stats(handle: Long, mode: Int)
    external fun input(handle: Long, buttons: Int, lx: Float, ly: Float, rx: Float, ry: Float,
        lt: Float, rt: Float, active: Boolean, gamepad: Boolean)
    external fun setInputMapping(handle: Long, values: String)
    external fun takeHaptic(handle: Long): Int
    external fun viewer(handle: Long, values: String): String
    external fun viewerCatalog(): String
    external fun validateViewer(values: String): String
}
