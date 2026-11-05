// SPDX-License-Identifier: Apache-2.0
@file:Suppress("DEPRECATION")
package com.pocketvr

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.ImageFormat
import android.hardware.Camera
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.widget.*
import com.google.zxing.BinaryBitmap
import com.google.zxing.PlanarYUVLuminanceSource
import com.google.zxing.common.HybridBinarizer
import com.google.zxing.qrcode.QRCodeReader

/** Camera is owned by one setup worker; never shared with the stream renderer. */
class QrScanActivity : Activity(), SurfaceHolder.Callback {
    companion object { @Volatile var cameraBusy = false; private set }
    private lateinit var preview: SurfaceView
    private lateinit var message: TextView
    private var worker: Handler? = null
    private var thread: HandlerThread? = null
    private var camera: Camera? = null // Only accessed on worker.
    private var active = false // UI thread only.
    private var surfaceReady = false
    private var delivered = false
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val body = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(20, 20, 20, 20) }
        message = TextView(this).apply { text = "Point the rear camera at the QR code. Camera stops when you leave this screen." }
        body.addView(message)
        preview = SurfaceView(this)
        preview.holder.addCallback(this)
        body.addView(preview, LinearLayout.LayoutParams(-1, 0, 1f))
        val input = EditText(this).apply { hint = "Or paste the full QR text"; maxLines = 3
            filters = arrayOf(android.text.InputFilter.LengthFilter(4096)) }
        body.addView(input)
        body.addView(Button(this).apply { text = "Use pasted code"; setOnClickListener { deliver(input.text.toString().trim()) } })
        body.addView(Button(this).apply { text = "Cancel"; setOnClickListener { finish() } })
        setContentView(body)
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED)
            requestPermissions(arrayOf(Manifest.permission.CAMERA), 1)
    }
    override fun onResume() { super.onResume(); active = true; startCamera() }
    override fun onPause() { active = false; stopCamera(); super.onPause() }
    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, results: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, results)
        if (requestCode == 1) {
            if (results.firstOrNull() == PackageManager.PERMISSION_GRANTED) startCamera()
            else message.text = "Camera permission was declined. You can paste the QR text or cancel."
        }
    }
    override fun surfaceCreated(holder: SurfaceHolder) { surfaceReady = true; startCamera() }
    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}
    override fun surfaceDestroyed(holder: SurfaceHolder) { surfaceReady = false; stopCamera() }
    private fun startCamera() {
        if (!active || !surfaceReady || worker != null || cameraBusy || delivered ||
            checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) return
        cameraBusy = true
        val t = HandlerThread("PocketVR-setup-camera").apply { start() }
        thread = t
        val handler = Handler(t.looper); worker = handler
        handler.post {
            try {
                val index = (0 until Camera.getNumberOfCameras()).firstOrNull {
                    Camera.CameraInfo().let { info -> Camera.getCameraInfo(it, info); info.facing == Camera.CameraInfo.CAMERA_FACING_BACK }
                } ?: throw IllegalStateException("No rear camera available")
                val cam = Camera.open(index); camera = cam
                val info = Camera.CameraInfo().also { Camera.getCameraInfo(index, it) }
                val parameters = cam.parameters
                val size = parameters.supportedPreviewSizes.filter { it.width <= 1280 && it.height <= 720 }
                    .minByOrNull { kotlin.math.abs(it.width - 640) + kotlin.math.abs(it.height - 480) }
                    ?: throw IllegalStateException("Camera has no supported low resolution preview")
                parameters.setPreviewSize(size.width, size.height)
                parameters.previewFormat = ImageFormat.NV21
                if (Camera.Parameters.FOCUS_MODE_CONTINUOUS_PICTURE in parameters.supportedFocusModes.orEmpty())
                    parameters.focusMode = Camera.Parameters.FOCUS_MODE_CONTINUOUS_PICTURE
                cam.parameters = parameters
                cam.setDisplayOrientation(info.orientation % 360) // Scanner locks to natural portrait.
                cam.setPreviewDisplay(preview.holder)
                val reader = QRCodeReader()
                var previous = 0L
                cam.setPreviewCallbackWithBuffer { data, current ->
                    try {
                        val now = SystemClock.elapsedRealtime()
                        if (now - previous >= 180 && data != null) {
                            previous = now
                            try {
                                val source = PlanarYUVLuminanceSource(data, size.width, size.height, 0, 0, size.width, size.height, false)
                                val result = reader.decode(BinaryBitmap(HybridBinarizer(source)))
                                val content = result.text
                                if (content.length in 1..4096) runOnUiThread { if (active) deliver(content) }
                            } catch (_: com.google.zxing.ReaderException) {
                                // No readable QR in this frame.
                            } finally { reader.reset() }
                        }
                    } finally { if (camera === current) current.addCallbackBuffer(data) }
                }
                cam.addCallbackBuffer(ByteArray(size.width * size.height * 3 / 2))
                cam.startPreview()
            } catch (failure: Exception) {
                closeCamera()
                runOnUiThread { if (active) message.text = "Camera unavailable: ${failure.message}. You can paste the QR text." }
            }
        }
    }
    private fun closeCamera() {
        camera?.let { cam ->
            try { cam.setPreviewCallbackWithBuffer(null); cam.stopPreview() } catch (_: RuntimeException) {}
            cam.release()
        }
        camera = null
        cameraBusy = false
    }
    private fun stopCamera() {
        val handler = worker ?: return
        val t = thread
        worker = null; thread = null
        handler.post { try { closeCamera() } finally { t?.quitSafely() } }
        // MainActivity gates Connect on cameraBusy until asynchronous release completes.
    }
    private fun deliver(text: String) {
        if (delivered) return
        if (text.isEmpty() || text.length > 4096) { message.text = "Paste a QR code of at most 4096 characters"; return }
        delivered = true
        stopCamera()
        setResult(RESULT_OK, Intent().putExtra("qr_text", text))
        finish()
    }
}
