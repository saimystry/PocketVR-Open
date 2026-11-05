// SPDX-License-Identifier: Apache-2.0
package com.pocketvr

import android.content.SharedPreferences

/** One complete manual profile; invalid edits never replace the saved profile. */
class ViewerProfiles(private val preferences: SharedPreferences) {
    fun saved(): String? = preferences.getString("manual_viewer", null)

    fun save(values: String): String {
        val canonical = NativeBridge.validateViewer(values)
        check(preferences.edit().putString("manual_viewer", canonical).commit()) {
            "Could not save calibration on this phone"
        }
        return canonical
    }
}
