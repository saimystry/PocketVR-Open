// SPDX-License-Identifier: Apache-2.0
package com.pocketvr.test

import com.pocketvr.NativeBridge

object NativeHandleChecks {
    @JvmStatic fun run() {
        val seed = ByteArray(32) { (it + 1).toByte() }
        val handle = try { NativeBridge.create(seed, "{\"codecs\":[]}") } finally { seed.fill(0) }
        check(handle != 0L)
        check(NativeBridge.status(handle).isNotEmpty())
        NativeBridge.input(handle,0,0f,0f,0f,0f,0f,0f,false,false)
        for (axis in listOf(Float.NaN,Float.POSITIVE_INFINITY,2f)) {
            var rejected = false
            try { NativeBridge.input(handle,0,axis,0f,0f,0f,0f,0f,true,true) }
            catch (_: IllegalStateException) { rejected = true }
            check(rejected) { "Invalid input axis accepted" }
        }
        var rejectedMapping = false
        try { NativeBridge.setInputMapping(handle,"{}") } catch (_: IllegalStateException) { rejectedMapping = true }
        check(rejectedMapping) { "Malformed mapping accepted" }
        NativeBridge.destroy(handle)
        for (stale in listOf(handle, 0L, Long.MAX_VALUE)) {
            var rejected = false
            try { NativeBridge.status(stale) } catch (_: IllegalStateException) { rejected = true }
            check(rejected) { "Stale/invalid native client handle was accepted" }
        }
    }
}
