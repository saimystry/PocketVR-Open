// SPDX-License-Identifier: Apache-2.0
package com.pocketvr.test

import android.content.Context
import android.util.Base64
import com.pocketvr.SecureStore
import java.security.KeyStore

object SecureStoreChecks {
    @JvmStatic fun run(context: Context) {
        val namespace = "paired_test_recovery"
        val alias = "PocketVR.test.recovery"
        val preferences = context.getSharedPreferences(namespace, Context.MODE_PRIVATE)
        try {
            preferences.edit().clear().commit()
            val original = SecureStore(context, namespace, alias)
            val first = original.identitySeed()
            original.trust("test", "ab".repeat(32))
            check(original.identitySeed().contentEquals(first))
            original.trust("usb", "ab".repeat(32))
            original.trust("other", "cd".repeat(32))
            original.forgetPc("usb")
            check(original.peer("test").isEmpty() && original.peer("usb").isEmpty())
            check(original.peer("other") == "cd".repeat(32))
            val sealed = Base64.decode(preferences.getString("sealed", ""), Base64.NO_WRAP)
            sealed[sealed.lastIndex] = (sealed.last().toInt() xor 1).toByte()
            preferences.edit().putString("sealed", Base64.encodeToString(sealed, Base64.NO_WRAP)).commit()
            val recovered = SecureStore(context, namespace, alias)
            val second = recovered.identitySeed()
            check(recovered.recoveredIdentity && !second.contentEquals(first))
            check(recovered.peer("test").isEmpty())
            recovered.forget("test")
            KeyStore.getInstance("AndroidKeyStore").apply { load(null); deleteEntry(alias) }
            val missingKey = SecureStore(context, namespace, alias)
            check(!missingKey.identitySeed().contentEquals(second) && missingKey.recoveredIdentity)
            first.fill(0); second.fill(0)
        } finally {
            preferences.edit().clear().commit()
            KeyStore.getInstance("AndroidKeyStore").apply { load(null); deleteEntry(alias) }
        }
    }
}
