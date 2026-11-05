// SPDX-License-Identifier: Apache-2.0
package com.pocketvr

import android.content.Context
import android.hardware.input.InputManager
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.os.Vibrator
import android.os.VibrationEffect
import android.util.SparseIntArray
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View

/** UI-thread producer. Heartbeat stops with lifecycle/focus; PC expires its input lease. */
class PhoneControls(context: Context, private val handle: () -> Long, private val enabled: () -> Boolean,
    private val volumeKeys: () -> Boolean = { true }) : InputManager.InputDeviceListener {
    private val handler = Handler(Looper.getMainLooper())
    private val manager = context.getSystemService(Context.INPUT_SERVICE) as InputManager
    @Suppress("DEPRECATION")
    private val vibrator = context.getSystemService(Context.VIBRATOR_SERVICE) as Vibrator
    private val axes = FloatArray(6)
    private val pointers = SparseIntArray()
    private var keys = 0
    private var device = -1
    private var running = false
    private var wasEnabled = false
    private var lastTap = 0L
    private var recenterUntil = 0L
    private val tick = object : Runnable {
        override fun run() {
            if (!running) return
            val active = enabled()
            if (active) send(true) else if (wasEnabled) release()
            wasEnabled = active
            val current=handle()
            if (current!=0L && active) try {
                var remaining=8
                while (remaining-- > 0) {
                    val event=NativeBridge.takeHaptic(current)
                    if (event==0) break
                    if (event!=0 && enabled()) {
                        val duration=event and 0xffff
                        val amplitude=(event ushr 16) and 0xff
                        if (duration==0||amplitude==0) vibrator.cancel()
                        else if (vibrator.hasVibrator()) vibrator.vibrate(VibrationEffect.createOneShot(duration.toLong(),
                            if (vibrator.hasAmplitudeControl()) amplitude else VibrationEffect.DEFAULT_AMPLITUDE))
                    }
                }
            } catch (_: RuntimeException) { vibrator.cancel() }
            handler.postDelayed(this, 16)
        }
    }
    fun start() {
        if (running) return
        running = true; manager.registerInputDeviceListener(this, handler); handler.post(tick)
    }
    fun stop() {
        running = false; wasEnabled=false; handler.removeCallbacks(tick); manager.unregisterInputDeviceListener(this); release()
    }
    fun release() {
        keys = 0; axes.fill(0f); pointers.clear(); device = -1; recenterUntil = 0; lastTap = 0
        vibrator.cancel()
        send(false)
    }
    private fun send(active: Boolean) {
        val current = handle()
        if (current == 0L) return
        var bits = keys
        for (i in 0 until pointers.size()) bits = bits or (1 shl pointers.valueAt(i))
        if (SystemClock.uptimeMillis() < recenterUntil) bits = bits or (1 shl 15)
        try { NativeBridge.input(current, bits, axes[0], axes[1], axes[2], axes[3], axes[4], axes[5], active, device != -1) }
        catch (_: RuntimeException) { /* Connection teardown may race one UI heartbeat. */ }
    }
    fun key(event: KeyEvent): Boolean {
        if (!enabled()) return false
        val volume = event.keyCode == KeyEvent.KEYCODE_VOLUME_UP || event.keyCode == KeyEvent.KEYCODE_VOLUME_DOWN
        if (volume && !volumeKeys()) return false
        if (!volume && event.source and InputDevice.SOURCE_GAMEPAD != InputDevice.SOURCE_GAMEPAD) return false
        val index = when(event.keyCode) {
            KeyEvent.KEYCODE_BUTTON_A -> 0; KeyEvent.KEYCODE_BUTTON_B -> 1
            KeyEvent.KEYCODE_BUTTON_X -> 2; KeyEvent.KEYCODE_BUTTON_Y -> 3
            KeyEvent.KEYCODE_BUTTON_L1 -> 4; KeyEvent.KEYCODE_BUTTON_R1 -> 5
            KeyEvent.KEYCODE_BUTTON_THUMBL -> 6; KeyEvent.KEYCODE_BUTTON_THUMBR -> 7
            KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_BUTTON_MODE -> 8
            KeyEvent.KEYCODE_BUTTON_SELECT -> 9
            KeyEvent.KEYCODE_VOLUME_UP -> 10; KeyEvent.KEYCODE_VOLUME_DOWN -> 11
            else -> return false
        }
        if (event.action != KeyEvent.ACTION_DOWN && event.action != KeyEvent.ACTION_UP) return false
        if (!volume) {
            if (device != -1 && device != event.deviceId) return false
            device = event.deviceId
        }
        keys = if (event.action == KeyEvent.ACTION_DOWN && !event.isCanceled) keys or (1 shl index) else keys and (1 shl index).inv()
        send(true); return true
    }
    private fun axis(event: MotionEvent, code: Int): Float {
        val value = event.getAxisValue(code)
        return if (value.isFinite()) value.coerceIn(-1f, 1f) else 0f
    }
    fun motion(event: MotionEvent): Boolean {
        if (!enabled() || event.source and InputDevice.SOURCE_JOYSTICK != InputDevice.SOURCE_JOYSTICK) return false
        if (device != -1 && device != event.deviceId) return false
        device = event.deviceId
        val input = event.device
        axes[0] = axis(event, MotionEvent.AXIS_X); axes[1] = -axis(event, MotionEvent.AXIS_Y)
        axes[2] = axis(event, if (input?.getMotionRange(MotionEvent.AXIS_Z) != null) MotionEvent.AXIS_Z else MotionEvent.AXIS_RX)
        axes[3] = -axis(event, if (input?.getMotionRange(MotionEvent.AXIS_RZ) != null) MotionEvent.AXIS_RZ else MotionEvent.AXIS_RY)
        axes[4] = axis(event, if (input?.getMotionRange(MotionEvent.AXIS_LTRIGGER) != null) MotionEvent.AXIS_LTRIGGER else MotionEvent.AXIS_BRAKE).coerceAtLeast(0f)
        axes[5] = axis(event, if (input?.getMotionRange(MotionEvent.AXIS_RTRIGGER) != null) MotionEvent.AXIS_RTRIGGER else MotionEvent.AXIS_GAS).coerceAtLeast(0f)
        send(true); return true
    }
    fun touch(view: View, event: MotionEvent): Boolean {
        if (!enabled()) return false
        when(event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val i = event.actionIndex
                val zone = if (event.getY(i) < view.height * .15f) 14 else if (event.getX(i) < view.width / 3f) 12 else 13
                pointers.put(event.getPointerId(i), zone)
                if (event.actionMasked == MotionEvent.ACTION_DOWN) {
                    val now = SystemClock.uptimeMillis()
                    if (lastTap != 0L && now - lastTap in 60..300) recenterUntil = now + 100
                    lastTap = now
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> pointers.delete(event.getPointerId(event.actionIndex))
            MotionEvent.ACTION_CANCEL -> pointers.clear()
        }
        send(true); return true
    }
    override fun onInputDeviceAdded(deviceId: Int) {}
    override fun onInputDeviceChanged(deviceId: Int) { if (deviceId == device) release() }
    override fun onInputDeviceRemoved(deviceId: Int) { if (deviceId == device) release() }
}
