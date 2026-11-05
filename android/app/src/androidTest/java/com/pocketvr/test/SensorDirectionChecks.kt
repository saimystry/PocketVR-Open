// SPDX-License-Identifier: Apache-2.0
package com.pocketvr.test

import android.hardware.SensorManager
import com.pocketvr.NativeTests
import kotlin.math.abs

object SensorDirectionChecks {
    @JvmStatic fun run() {
        val axes = listOf(
            SensorManager.AXIS_X to SensorManager.AXIS_Y,
            SensorManager.AXIS_Y to SensorManager.AXIS_MINUS_X,
            SensorManager.AXIS_MINUS_X to SensorManager.AXIS_MINUS_Y,
            SensorManager.AXIS_MINUS_Y to SensorManager.AXIS_X
        )
        val identity = floatArrayOf(1f,0f,0f,0f,1f,0f,0f,0f,1f)
        val raw = doubleArrayOf(0.2, -0.3, 0.4)
        for ((rotation, pair) in axes.withIndex()) {
            val worldFromDisplay = FloatArray(9)
            check(SensorManager.remapCoordinateSystem(identity, pair.first, pair.second, worldFromDisplay))
            val actual = NativeTests.sensorVector(raw[0], raw[1], raw[2], rotation)
            // The platform matrix maps display vectors into the sensor/world
            // reference. Its transpose maps raw gyro/accel into display axes.
            for (i in 0..2) {
                val expected = (0..2).sumOf { row -> worldFromDisplay[row * 3 + i] * raw[row] }
                check(abs(actual[i] - expected) < 1e-9) { "Sensor direction disagrees with Android at rotation $rotation" }
            }
        }
    }
}
