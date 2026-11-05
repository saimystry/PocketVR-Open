// SPDX-License-Identifier: Apache-2.0
package com.pocketvr

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import org.json.JSONObject
import java.security.KeyStore
import java.security.SecureRandom
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

class SecureStore(context: Context, namespace: String = "paired", private val alias: String = "PocketVR.Identity.v1") {
    private val preferences = context.getSharedPreferences(namespace, Context.MODE_PRIVATE)
    var recoveredIdentity = false
        private set
    private fun key(): SecretKey {
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (store.getKey(alias, null) as? SecretKey)?.let { return it }
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").apply {
            init(KeyGenParameterSpec.Builder(alias,
                KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setRandomizedEncryptionRequired(true).build())
        }.generateKey()
    }
    private fun read(): JSONObject {
        val encoded = preferences.getString("sealed", null) ?: return JSONObject()
        val bytes = Base64.decode(encoded, Base64.NO_WRAP)
        require(bytes.size >= 28) { "Paired-device storage is truncated" }
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, bytes.copyOfRange(0, 12)))
        return JSONObject(String(cipher.doFinal(bytes, 12, bytes.size - 12), Charsets.UTF_8))
    }
    private fun write(value: JSONObject) {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, key())
        val sealed = cipher.iv + cipher.doFinal(value.toString().toByteArray(Charsets.UTF_8))
        check(preferences.edit().putString("sealed", Base64.encodeToString(sealed, Base64.NO_WRAP)).commit()) {
            "Unable to save paired-device storage"
        }
    }
    fun identitySeed(): ByteArray {
        val data = try {
            read().also { data ->
                if (data.has("seed")) require(Base64.decode(data.getString("seed"), Base64.NO_WRAP).size == 32)
            }
        } catch (error: Exception) {
            // Corrupt/restored ciphertext or an invalidated key cannot retain
            // identity trust. Clear only this store, then require normal pairing.
            if (error !is java.security.GeneralSecurityException &&
                error !is IllegalArgumentException && error !is org.json.JSONException) throw error
            check(preferences.edit().remove("sealed").commit()) { "Unable to reset unreadable pairing storage" }
            KeyStore.getInstance("AndroidKeyStore").apply { load(null); deleteEntry(alias) }
            recoveredIdentity = true
            JSONObject()
        }
        if (!data.has("seed")) {
            val seed = ByteArray(32).also { SecureRandom().nextBytes(it) }
            data.put("seed", Base64.encodeToString(seed, Base64.NO_WRAP))
            write(data)
        }
        return Base64.decode(data.getString("seed"), Base64.NO_WRAP).also { require(it.size == 32) }
    }
    fun peer(ip: String): String = read().optJSONObject("peers")?.optString(ip, "") ?: ""
    fun trust(ip: String, identity: String) {
        require(identity.matches(Regex("[0-9a-f]{64}")))
        val data = read()
        val peers = data.optJSONObject("peers") ?: JSONObject()
        peers.put(ip, identity)
        data.put("peers", peers)
        write(data)
    }
    fun forget(ip: String) {
        val data = read()
        data.optJSONObject("peers")?.remove(ip)
        write(data)
    }
    fun forgetPc(ip: String) {
        val data = read()
        val peers = data.optJSONObject("peers")
        val identity = peers?.optString(ip, "").orEmpty()
        if (identity.isNotEmpty()) {
            peers?.keys()?.asSequence()?.toList()?.filter { peers.optString(it) == identity }
                ?.forEach { peers.remove(it) }
        } else peers?.remove(ip)
        write(data)
    }
}
