// SPDX-License-Identifier: Apache-2.0
package com.pocketvr

import android.app.Activity
import android.app.AlertDialog
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.Color
import android.hardware.SensorManager
import android.media.MediaCodecList
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.net.wifi.WifiManager
import android.os.BatteryManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.view.View
import android.view.WindowManager
import android.view.SurfaceView
import android.graphics.SurfaceTexture
import android.content.pm.ActivityInfo
import android.widget.*
import org.json.JSONArray
import org.json.JSONObject
import java.net.Inet4Address

class MainActivity : Activity() {
    private val phoneControls by lazy { PhoneControls(this, { nativeHandle }, { resumed && headset && hasWindowFocus() }, { volumeKeys }) }
    private val handler = Handler(Looper.getMainLooper())
    private var nativeHandle = 0L
    private lateinit var store: SecureStore
    private lateinit var address: EditText
    private lateinit var port: EditText
    private lateinit var status: TextView
    private lateinit var devices: LinearLayout
    private lateinit var viewerSummary: TextView
    private lateinit var viewerProfiles: ViewerProfiles
    private var viewerValues: JSONObject? = null
    private lateinit var preview: SurfaceView
    private lateinit var setupView: ScrollView
    private lateinit var previewColumn: LinearLayout
    private lateinit var previewFrame: android.widget.FrameLayout
    private lateinit var hero: View
    private lateinit var ui: Ui
    private lateinit var homePage: LinearLayout
    private lateinit var settingsPage: LinearLayout
    private lateinit var stateDot: View
    private lateinit var headline: TextView
    private lateinit var subline: TextView
    private lateinit var connectButton: Button
    private lateinit var stopButton: Button
    private lateinit var vrButton: Button
    private lateinit var quickActions: View
    private lateinit var devicesCard: View
    private lateinit var manualFields: View
    private var noticeUntil = 0L
    private var deviceList = ""
    private var viewMode = 0
    private var eyeSize = 135
    private var gridOn = false
    private var previewing = false
    private var adoptedLenses = ""
    private lateinit var previewBar: View
    private lateinit var eyeSizeRow: View
    private var volumeKeys = true
    private var decoderTexture: SurfaceTexture? = null
    private var headset = false
    private var statsMode = -1
    private var usbAdb = false
    private var automatic = true
    private lateinit var autoSwitch: Switch
    private var selectedPin = ""
    private var routeLabel = ""
    private var attemptAt = 0L
    private var lastConnected = false
    private val attempted = linkedMapOf<String, Long>()
    private var keepAwake = true
    private lateinit var usbSwitch: Switch
    private var previousBrightness = WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
    var viewerDialog: AlertDialog? = null
        private set
    private var desiredConnection = false
    private var resumed = false
    private var peerSaved = ""
    private var connectedIp = ""
    private var connectedPort = 9943
    private var activeTcp = false
    private var wifiLock: WifiManager.WifiLock? = null
    private var wakeLock: PowerManager.WakeLock? = null
    private var discovering = false
    private val nsd by lazy { getSystemService(Context.NSD_SERVICE) as NsdManager }
    private data class Candidate(val ip: String, val port: Int, val name: String, val seen: Long, val identity: String = "", val tether: Boolean = false)
    private val discovered = linkedMapOf<String, Candidate>()
    private fun dp(value: Int) = (value * resources.displayMetrics.density).toInt()
    private fun button(value: String, action: () -> Unit) = Button(this).apply {
        text = value; isAllCaps = false; setOnClickListener { action() }
    }
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        ui = Ui(this)
        val prefs = getPreferences(MODE_PRIVATE)
        (getSystemService(DISPLAY_SERVICE) as android.hardware.display.DisplayManager)
            .registerDisplayListener(displayListener, handler)
        automatic = prefs.getBoolean("automatic_transport", true)
        usbAdb = prefs.getBoolean("usb_adb", false)
        statsMode = prefs.getInt("stats_overlay", -1).coerceIn(-1, 2)
        viewMode = prefs.getInt("view_mode", 0).coerceIn(0, 2)
        eyeSize = prefs.getInt("eye_size", 135).coerceIn(70, 160)
        volumeKeys = prefs.getBoolean("volume_keys", true)
        // Keep the SurfaceView outside scrolling/status layout. Otherwise a
        // longer "connected" status can clip it out, destroy its surface and
        // stop the very connection that produced that status.
        previewColumn = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Ui.BACKGROUND)
        }
        preview = SurfaceView(this)
        preview.setOnTouchListener { view, event -> phoneControls.touch(view,event) }
        hero = ui.hero()
        previewFrame = android.widget.FrameLayout(this).apply {
            setBackgroundColor(Color.BLACK)
            addView(preview, android.widget.FrameLayout.LayoutParams(-1, -1))
            addView(hero, android.widget.FrameLayout.LayoutParams(-1, -1))
        }
        previewBar = previewControls()
        previewFrame.addView(previewBar, android.widget.FrameLayout.LayoutParams(-1, -2, android.view.Gravity.BOTTOM))
        homePage = ui.column(20)
        settingsPage = ui.column(20).apply { visibility = View.GONE }
        val pages = ui.column().apply { addView(homePage); addView(settingsPage) }
        setupView = ScrollView(this).apply { addView(pages); isFillViewport = true }
        previewColumn.addView(previewFrame, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(200)))
        previewColumn.addView(setupView, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))
        setContentView(previewColumn)
        buildHome()
        buildSettings()
        preview.holder.addCallback(object : android.view.SurfaceHolder.Callback {
            override fun surfaceCreated(holder: android.view.SurfaceHolder) {
                attachSurface()
            }
            override fun surfaceChanged(holder: android.view.SurfaceHolder, format: Int, width: Int, height: Int) {}
            override fun surfaceDestroyed(holder: android.view.SurfaceHolder) {
                detachSurface()
            }
        })
        try {
            store = SecureStore(this)
            val seed = store.identitySeed()
            try { nativeHandle = NativeBridge.create(seed, capabilities().toString()) }
            finally { seed.fill(0) }
            if (store.recoveredIdentity) say("Connection details were reset. Tap Connect.")
            check(nativeHandle != 0L) { "Native client initialization failed" }
            updateThermal()
            NativeBridge.stats(nativeHandle, statsMode)
            NativeBridge.viewMode(nativeHandle, viewMode)
            NativeBridge.viewSize(nativeHandle, eyeSize / 100.0)
            viewerProfiles = ViewerProfiles(getSharedPreferences("viewer", MODE_PRIVATE))
            viewerProfiles.saved()?.let { saved ->
                try {
                    viewerValues = JSONObject(NativeBridge.viewer(nativeHandle, saved))
                    viewerSummary.text = "Saved on phone; waiting for PC"
                } catch (_: Exception) {
                    viewerSummary.text = "Saved lens settings are invalid. Open Adjust lenses to fix them."
                }
            }
        } catch (error: Exception) {
            say("Setup failed: ${error.message}")
        }
    }
    /** One status line, one main button: everything else lives in Settings. */
    private fun buildHome() {
        stateDot = ui.dot()
        headline = ui.label("Ready to connect", 19f, Ui.TEXT, true)
        subline = ui.label("Open PocketVR on your PC, then tap Connect.", 14f, Ui.MUTED)
        val top = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL; gravity = android.view.Gravity.CENTER_VERTICAL
            addView(stateDot); addView(headline)
        }
        homePage.addView(ui.card(top, subline.apply { setPadding(0, dp(6), 0, 0) }))
        devices = ui.column()
        devicesCard = ui.card(ui.label("Choose your PC", 16f, Ui.TEXT, true), devices).apply { visibility = View.GONE }
        homePage.addView(devicesCard)
        connectButton = ui.primary("Connect") { desiredConnection = true; attempted.clear(); selectedPin = ""; connect() }.apply { tag = "connect" }
        vrButton = ui.primary("Enter VR") { headsetView(true) }.apply { tag = "headset_view"; visibility = View.GONE }
        stopButton = ui.quiet("Cancel") { desiredConnection = false; if (nativeHandle != 0L) NativeBridge.stop(nativeHandle); power(false) }.apply { visibility = View.GONE }
        quickActions = ui.pair(
            ui.secondary("Recenter") { if (nativeHandle != 0L) NativeBridge.recenter(nativeHandle, false) },
            ui.secondary("Calibrate") {
                if (nativeHandle != 0L) NativeBridge.recenter(nativeHandle, true)
                say("Keep the phone still for 2 seconds.")
            }).apply { visibility = View.GONE }
        homePage.addView(connectButton); homePage.addView(vrButton); homePage.addView(quickActions); homePage.addView(stopButton)
        homePage.addView(ui.quiet("Settings") { page(true) }.apply { tag = "settings" })
    }
    private fun buildSettings() {
        val prefs = getPreferences(MODE_PRIVATE)
        settingsPage.addView(ui.quiet("\u2039  Back") { page(false) })
        settingsPage.addView(ui.label("Settings", 26f, Ui.TEXT, true).apply { setPadding(dp(4), dp(4), 0, dp(4)) })

        address = ui.field("PC address, for example 192.168.0.10", android.text.InputType.TYPE_CLASS_PHONE,
            prefs.getString("pc", "").orEmpty(), "pc_address")
        port = ui.field("Port", android.text.InputType.TYPE_CLASS_NUMBER, prefs.getInt("port", 9943).toString(), "pc_port")
        if (usbAdb) { address.setText("127.0.0.1"); address.isEnabled = false }
        usbSwitch = Switch(this).apply {
            tag = "usb_adb"
            isChecked = usbAdb
            setOnCheckedChangeListener { _, enabled ->
                if (::autoSwitch.isInitialized) autoSwitch.isChecked = false
                desiredConnection = false
                if (nativeHandle != 0L) { NativeBridge.stop(nativeHandle); power(false) }
                if (enabled) {
                    if (address.text.toString() != "127.0.0.1")
                        getPreferences(MODE_PRIVATE).edit().putString("wifi_pc", address.text.toString()).apply()
                    address.setText("127.0.0.1")
                } else address.setText(getPreferences(MODE_PRIVATE).getString("wifi_pc", ""))
                address.isEnabled = !enabled
                usbAdb = enabled
                getPreferences(MODE_PRIVATE).edit().putBoolean("usb_adb", enabled).apply()
            }
        }
        autoSwitch = Switch(this).apply {
            tag = "automatic_transport"
            isChecked = automatic
            setOnCheckedChangeListener { _, enabled ->
                automatic = enabled
                desiredConnection = false
                attempted.clear()
                if (nativeHandle != 0L) NativeBridge.stop(nativeHandle)
                power(false)
                address.isEnabled = !enabled && !usbAdb
                manualFields.visibility = if (enabled) View.GONE else View.VISIBLE
                getPreferences(MODE_PRIVATE).edit().putBoolean("automatic_transport", enabled).apply()
            }
        }
        address.isEnabled = !automatic && !usbAdb
        manualFields = ui.column().apply {
            addView(ui.toggle("USB cable (adb)", "Uses USB debugging", usbSwitch))
            addView(address); addView(port)
            visibility = if (automatic) View.GONE else View.VISIBLE
        }
        settingsPage.addView(ui.section("Connection"))
        settingsPage.addView(ui.card(
            ui.toggle("Connect automatically", "Picks USB when plugged in, otherwise Wi-Fi", autoSwitch),
            manualFields,
            ui.row("Scan the code on your PC") { scanQr(4101) }.apply { tag = "scan_pc_qr" },
            ui.row("USB tethering", "Fastest cable connection") {
                try { startActivity(Intent("android.settings.TETHER_SETTINGS")) }
                catch (_: android.content.ActivityNotFoundException) { startActivity(Intent(android.provider.Settings.ACTION_WIRELESS_SETTINGS)) }
            },
            ui.row("Forget this PC", "Use this if you change PCs") { forgetPc() }))

        viewerSummary = ui.label("Standard headset profile", 13f, Ui.MUTED)
        settingsPage.addView(ui.section("Headset"))
        settingsPage.addView(ui.card(viewerSummary,
            ui.row("Scan headset QR code", "Sets up your lenses automatically") { scanQr(4102) }.apply { tag = "scan_viewer_qr" },
            ui.row("Adjust lenses by hand", "Lens distance, spacing and field of view") { viewerCalibration() }.apply { tag = "viewer_calibration" },
            ui.row("Preview and eye size", "${eyeSize}%. See the view and make each eye bigger or smaller") { lensPreview(true) }.apply { tag = "lens_preview"; eyeSizeRow = this },
            ui.toggle("Alignment grid", "Shows a grid while you line up the lenses", Switch(this).apply {
                setOnCheckedChangeListener { _, enabled -> gridOn = enabled; if (nativeHandle != 0L) NativeBridge.grid(nativeHandle, enabled) }
            })))

        settingsPage.addView(ui.section("Display"))
        settingsPage.addView(ui.card(
            ui.row("Picture quality", "Resolution, antialiasing, bitrate and more") { pictureSettings() }.apply { tag = "picture_settings" },
            ui.choice("View", arrayOf("Headset", "Fit to screen", "No headset (single view)"), viewMode) { mode ->
                viewMode = mode
                getPreferences(MODE_PRIVATE).edit().putInt("view_mode", mode).apply()
                if (nativeHandle != 0L) NativeBridge.viewMode(nativeHandle, mode)
            },
            ui.choice("Performance overlay", arrayOf("Same as PC", "Off", "Minimal", "Full"), statsMode + 1) { position ->
                statsMode = position - 1
                getPreferences(MODE_PRIVATE).edit().putInt("stats_overlay", statsMode).apply()
                if (nativeHandle != 0L) NativeBridge.stats(nativeHandle, statsMode)
            }))

        settingsPage.addView(ui.section("Controls"))
        settingsPage.addView(ui.card(
            ui.row("Button mapping", "Gamepad, touch and volume buttons") { inputMapping() }.apply { tag = "input_mapping" },
            ui.toggle("Volume buttons control VR", "Turn off to keep normal phone volume", Switch(this).apply {
                isChecked = volumeKeys
                setOnCheckedChangeListener { _, enabled ->
                    volumeKeys = enabled
                    getPreferences(MODE_PRIVATE).edit().putBoolean("volume_keys", enabled).apply()
                }
            }),
            ui.toggle("Faster display (experimental)", "Can lower delay, but may tear. Applies next time VR opens", Switch(this).apply {
                isChecked = getPreferences(MODE_PRIVATE).getBoolean("single_buffer", false)
                setOnCheckedChangeListener { _, enabled ->
                    getPreferences(MODE_PRIVATE).edit().putBoolean("single_buffer", enabled).apply()
                }
            })))

        status = ui.label("Not connected", 13f, Ui.MUTED)
        settingsPage.addView(ui.section("Details"))
        settingsPage.addView(ui.card(status,
            ui.row("Share logs", "Send what the app recorded, for help with a problem") { shareLogs() }.apply { tag = "share_logs" }))
        settingsPage.addView(ui.label("Game audio plays on your PC. For Wi-Fi, use a 5 GHz network.", 13f, Ui.MUTED).apply { setPadding(dp(4), dp(8), dp(4), dp(8)) })
    }
    private fun page(settings: Boolean) {
        homePage.visibility = if (settings) View.GONE else View.VISIBLE
        settingsPage.visibility = if (settings) View.VISIBLE else View.GONE
        setupView.scrollTo(0, 0)
    }
    /** Shows a short message on the home screen for a few seconds. */
    private fun say(message: String?) {
        subline.text = message.orEmpty()
        noticeUntil = android.os.SystemClock.elapsedRealtime() + 5000
    }
    private fun forgetPc() {
        AlertDialog.Builder(this).setTitle("Forget this PC?")
            .setMessage("PocketVR will connect to the next PC it finds.")
            .setNegativeButton("Cancel", null).setPositiveButton("Forget") { _, _ ->
                desiredConnection = false
                if (nativeHandle != 0L) NativeBridge.stop(nativeHandle)
                try {
                    check(::store.isInitialized) { "Pairing storage is unavailable; reopen PocketVR" }
                    store.forgetPc(connectedIp.ifEmpty { address.text.toString().trim() })
                    selectedPin = ""
                    attempted.clear()
                    peerSaved = ""
                } catch (error: Exception) { say("Could not forget PC: ${error.message}") }
            }.show()
    }
    /** Turns the native client state into the plain words and buttons on the home screen. */
    private fun showState(value: JSONObject, connected: Boolean) {
        val error = value.optString("error")
        val streaming = connected && (value.optJSONObject("decoder")?.optLong("decoded") ?: 0L) > 0
        val calibrating = connected && value.optJSONObject("sensors")?.optBoolean("calibrating") == true
        val (color, title, detail) = when {
            calibrating -> Triple(Ui.WARN, "Hold still", "Keep the phone still for 2 seconds.")
            value.optBoolean("format_changing") -> Triple(Ui.WARN, "Adjusting quality", "The picture will be back in a moment.")
            streaming -> Triple(Ui.GOOD, "Connected", if (routeLabel.isNotEmpty()) "Streaming over $routeLabel" else "Streaming")
            connected -> Triple(Ui.WARN, "Connected", "Waiting for SteamVR. Start a game or SteamVR on your PC.")
            error.isNotEmpty() -> Triple(Ui.BAD, "Couldn't connect", error)
            desiredConnection && value.optBoolean("approval") ->
                Triple(Ui.WARN, "Allow this phone", "If your PC asks, click Allow in the PocketVR app there.")
            desiredConnection -> Triple(Ui.ACCENT, "Connecting", "Looking for your PC.")
            else -> Triple(Ui.MUTED, "Ready to connect", "Open PocketVR on your PC, then tap Connect.")
        }
        ui.tint(stateDot, color)
        headline.text = title
        if (android.os.SystemClock.elapsedRealtime() > noticeUntil) subline.text = detail
        hero.visibility = if (streaming || headset) View.GONE else View.VISIBLE
        connectButton.visibility = if (!connected && (!desiredConnection || error.isNotEmpty())) View.VISIBLE else View.GONE
        connectButton.text = if (error.isNotEmpty()) "Try again" else "Connect"
        vrButton.visibility = if (connected) View.VISIBLE else View.GONE
        quickActions.visibility = if (connected) View.VISIBLE else View.GONE
        stopButton.visibility = if (connected || desiredConnection) View.VISIBLE else View.GONE
        stopButton.text = if (connected) "Disconnect" else "Cancel"
        devicesCard.visibility = if (!connected && selectedPin.isEmpty() &&
            discovered.values.map { it.identity.ifEmpty { it.ip } }.distinct().size > 1) View.VISIBLE else View.GONE
    }
    private fun attachSurface() {
        if (nativeHandle == 0L || !resumed || decoderTexture != null || !preview.holder.surface.isValid) return
        val texture = SurfaceTexture(false)
        try {
            NativeBridge.surface(nativeHandle, preview.holder.surface, texture,
                getPreferences(MODE_PRIVATE).getBoolean("single_buffer", false))
            decoderTexture = texture
            if (desiredConnection) {
                if (automatic && connectedIp.isNotEmpty())
                    connectEndpoint(connectedIp, connectedPort, selectedPin)
                else connect()
            }
        } catch (failure: Exception) {
            texture.release()
            say("Renderer setup failed: ${failure.message}")
        }
    }
    private fun detachSurface() {
        if (decoderTexture == null) return
        if (nativeHandle != 0L) NativeBridge.surface(nativeHandle, null, null, false)
        decoderTexture?.release()
        decoderTexture = null
    }
    @Suppress("DEPRECATION")
    /** Settings that live on the PC, changed from the phone while connected. */
    private fun pictureSettings() {
        val saved = if (nativeHandle != 0L) JSONObject(NativeBridge.status(nativeHandle)).optJSONObject("settings") else null
        if (saved == null) {
            Toast.makeText(this, "Connect to your PC to change these", Toast.LENGTH_LONG).show(); return
        }
        val schema = JSONObject(NativeBridge.viewerCatalog()).getJSONObject("schema").getJSONObject("properties")
        fun spec(section: String, name: String) = schema.getJSONObject(section).getJSONObject("properties").getJSONObject(name)
        fun send(section: String, name: String, value: String) = NativeBridge.setting(nativeHandle, "$section.$name", value)
        fun pick(title: String, section: String, name: String): View {
            val options = spec(section, name).getJSONArray("enum")
            val words = Array(options.length()) { options.get(it).toString().replaceFirstChar { c -> c.uppercase() } }
            var current = 0
            for (i in 0 until options.length()) if (options.get(i).toString() == saved.getJSONObject(section).get(name).toString()) current = i
            return ui.choice(title, words, current) { send(section, name, JSONObject.wrap(options.get(it)).let { v -> if (v is String) JSONObject.quote(v) else v.toString() }) }
        }
        fun slide(title: String, section: String, name: String, step: Double, scale: Double, unit: String): View {
            val rule = spec(section, name)
            val low = rule.getDouble("minimum")
            val steps = Math.round((rule.getDouble("maximum") - low) / step).toInt()
            val at = Math.round((saved.getJSONObject(section).getDouble(name) - low) / step).toInt()
            return ui.slider(title, steps, at, "$section.$name", { "${Math.round((low + it * step) * scale)}$unit" }) {
                send(section, name, java.math.BigDecimal.valueOf(low + it * step).setScale(4, java.math.RoundingMode.HALF_UP).stripTrailingZeros().toPlainString())
            }
        }
        val listed = JSONObject(NativeBridge.status(nativeHandle)).optJSONObject("profiles")
        val profiles = buildList {
            for (group in listOf("built_in", "saved")) {
                val names = listed?.optJSONArray(group) ?: continue
                for (i in 0 until names.length()) add(names.getString(i))
            }
        }
        val body = ui.column(20).apply {
            if (profiles.isNotEmpty())
                addView(ui.choice("Profile", (listOf("Choose one") + profiles).toTypedArray(), 0) { index ->
                    if (index > 0) NativeBridge.setting(nativeHandle, "profile", JSONObject.quote(profiles[index - 1]))
                })
            addView(pick("Stream resolution", "video", "resolution"))
            addView(ui.label("Applies the next time the phone connects.", 13f, Ui.MUTED).apply { setPadding(0, 0, 0, dp(12)) })
            addView(slide("Antialiasing (supersampling)", "video", "render_scale", 0.05, 100.0, "%"))
            addView(ui.label("Smoother edges but slower. Restart SteamVR if it does not change.", 13f, Ui.MUTED).apply { setPadding(0, 0, 0, dp(12)) })
            addView(slide("Bitrate", "video", "bitrate_mbps", 1.0, 1.0, " Mbps"))
            addView(pick("Frame rate", "video", "fps"))
            addView(pick("Video format", "video", "codec"))
            addView(slide("Edge darkening", "display", "vignette_strength", 0.01, 100.0, "%"))
        }
        AlertDialog.Builder(this).setTitle("Picture quality").setView(ScrollView(this).apply { addView(body) })
            .setPositiveButton("Done", null).show()
    }
    /** Changes made in the PC app are kept here too, or they would be lost at the next connection. */
    private fun adoptFromPc(value: JSONObject) {
        val eye = value.optDouble("eye_size")
        if (!eye.isNaN()) {
            val percent = Math.round(eye * 100).toInt()
            if (percent != eyeSize) {
                eyeSize = percent
                getPreferences(MODE_PRIVATE).edit().putInt("eye_size", percent).apply()
                eyeSizeRow.findViewWithTag<TextView>("detail").text = "${percent}%. See the view and make each eye bigger or smaller"
                previewBar.findViewWithTag<SeekBar>("eye_size").progress = percent - 70
            }
        }
        val lenses = value.optJSONObject("viewer_from_pc") ?: return
        val text = lenses.toString()
        if (text != adoptedLenses && ::viewerProfiles.isInitialized) {
            adoptedLenses = text
            try {
                viewerValues = JSONObject(viewerProfiles.save(text))
            } catch (_: RuntimeException) {}
        }
    }
    private fun shareLogs() {
        Thread {
            val text = try {
                val process = ProcessBuilder("logcat", "-d", "-t", "400", "--pid=${android.os.Process.myPid()}")
                    .redirectErrorStream(true).start()
                process.inputStream.bufferedReader().use { it.readText() }
            } catch (error: java.io.IOException) { "Logs unavailable: ${error.message}" }
            val summary = "PocketVR on ${Build.MANUFACTURER} ${Build.MODEL}, Android ${Build.VERSION.RELEASE}\n\n"
            runOnUiThread {
                val send = Intent(Intent.ACTION_SEND).setType("text/plain")
                    .putExtra(Intent.EXTRA_SUBJECT, "PocketVR logs")
                    .putExtra(Intent.EXTRA_TEXT, summary + text.takeLast(60000))
                startActivity(Intent.createChooser(send, "Share logs"))
            }
        }.start()
    }
    private fun setEyeSize(percent: Int) {
        eyeSize = percent
        getPreferences(MODE_PRIVATE).edit().putInt("eye_size", percent).apply()
        if (nativeHandle != 0L) NativeBridge.viewSize(nativeHandle, percent / 100.0)
        eyeSizeRow.findViewWithTag<TextView>("detail").text = "${percent}%. See the view and make each eye bigger or smaller"
    }
    private fun previewControls(): View {
        val bar = ui.slider("Size of each eye", 90, eyeSize - 70, "eye_size", { "${it + 70}%" }) { setEyeSize(it + 70) }
        return ui.column(12).apply {
            setBackgroundColor(Color.argb(215, 11, 15, 23))
            visibility = View.GONE
            addView(bar)
            addView(ui.pair(ui.secondary("Reset") {
                bar.findViewWithTag<SeekBar>("eye_size").progress = 65
                setEyeSize(135)
            }, ui.primary("Done") { lensPreview(false) }))
        }
    }
    /** Works with no PC: the grid shows the same edges a game would have. */
    private fun lensPreview(enabled: Boolean) {
        previewing = enabled
        previewBar.visibility = if (enabled) View.VISIBLE else View.GONE
        val streaming = vrButton.visibility == View.VISIBLE
        if (nativeHandle != 0L) NativeBridge.grid(nativeHandle, gridOn || (enabled && !streaming))
        headsetView(enabled)
    }
    private fun headsetView(enabled: Boolean) {
        if (headset == enabled) return
        headset = enabled
        phoneControls.release()
        // Keep the same SurfaceView attached to the same window. Reparenting
        // destroys its surface and therefore tears down decoder and transport.
        setupView.visibility = if (enabled) View.GONE else View.VISIBLE
        previewFrame.layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
            if (enabled) LinearLayout.LayoutParams.MATCH_PARENT else dp(200))
        if (enabled) hero.visibility = View.GONE
        if (enabled) {
            // Headsets hold the phone either way round. Locking one direction leaves the
            // picture and head tracking upside down when it goes in the other way.
            requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
            window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or View.SYSTEM_UI_FLAG_FULLSCREEN or
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
            if (keepAwake) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            // Without this the view stops short of the camera cutout, which shifts
            // both eyes away from the lenses.
            window.attributes = window.attributes.apply {
                preferredRefreshRate = 60f
                layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
            }
        } else {
            requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
            window.decorView.systemUiVisibility = 0
            window.attributes = window.attributes.apply {
                layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_DEFAULT
            }
            if (wakeLock == null) window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        }
    }
    @Suppress("DEPRECATION")
    override fun onBackPressed() {
        if (previewing) lensPreview(false)
        else if (headset) headsetView(false)
        else if (settingsPage.visibility == View.VISIBLE) page(false)
        else super.onBackPressed()
    }
    private fun inputMapping() {
        if (nativeHandle == 0L) return
        try {
            val state = JSONObject(NativeBridge.status(nativeHandle))
            if (!state.optBoolean("connected") || !state.optBoolean("phone_input_supported")) {
                Toast.makeText(this, "Connect to an updated PC to edit its saved mapping", Toast.LENGTH_LONG).show(); return
            }
            val buttons = arrayOf("a","b","x","y","left_bumper","right_bumper","left_stick","right_stick",
                "start","select","volume_up","volume_down","touch_left","touch_right","touch_menu","recenter")
            val actions = arrayOf("none","left_trigger","right_trigger","left_grip","right_grip","left_menu","right_menu",
                "system","left_pad","right_pad","recenter")
            val presets = arrayOf("xbox","playstation","one_button_cardboard","touchscreen","custom")
            val values = state.getJSONObject("settings").getJSONObject("input")
            val body = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(16),0,dp(16),0) }
            body.addView(TextView(this).apply { text = "Choose a preset, or pick Custom to set each button. Double tap the screen to recenter." })
            fun choice(label: String, choices: Array<String>, current: String): Spinner {
                body.addView(TextView(this).apply { text = label })
                return Spinner(this).apply {
                    adapter = ArrayAdapter(this@MainActivity, android.R.layout.simple_spinner_dropdown_item, choices)
                    setSelection(choices.indexOf(current).coerceAtLeast(0)); body.addView(this)
                }
            }
            val preset = choice("Preset", presets, values.getString("mapping"))
            val bindings = buttons.map { choice(it.replace('_',' ').replaceFirstChar(Char::uppercase),actions,values.getString("bind_$it")) }
            val dialog = AlertDialog.Builder(this).setTitle("Phone input mapping")
                .setView(ScrollView(this).apply { addView(body) }).setNegativeButton("Cancel",null).setPositiveButton("Apply",null).create()
            dialog.setOnShowListener { dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                try {
                    val next = JSONObject().put("mapping", presets[preset.selectedItemPosition])
                    buttons.forEachIndexed { i,name -> next.put("bind_$name",actions[bindings[i].selectedItemPosition]) }
                    NativeBridge.setInputMapping(nativeHandle,next.toString()); dialog.dismiss()
                    Toast.makeText(this,"Saving mapping; watch PC acknowledgement below",Toast.LENGTH_LONG).show()
                } catch (error: RuntimeException) { Toast.makeText(this,error.message,Toast.LENGTH_LONG).show() }
            } }
            dialog.show()
        } catch (error: RuntimeException) { Toast.makeText(this,error.message,Toast.LENGTH_LONG).show() }
    }
    private fun viewerCalibration() {
        if (nativeHandle == 0L || !::viewerProfiles.isInitialized) return
        if (viewerDialog?.isShowing == true) return
        val catalog = JSONObject(NativeBridge.viewerCatalog())
        val defaults = catalog.getJSONObject("values")
        val values = JSONObject((viewerValues ?: defaults).toString())
        data class Field(val key: String, val label: String, val scale: Double, val step: Double, val unit: String, val main: Boolean = false)
        val fields = listOf(
            Field("screen_to_lens_m", "Lens to screen", 1000.0, 0.1, " mm", true),
            Field("inter_lens_distance_m", "Space between lenses", 1000.0, 0.1, " mm", true),
            Field("fov_scale", "Field of view", 100.0, 1.0, "%", true),
            Field("ipd_mm", "Space between your eyes", 1.0, 0.1, " mm"),
            Field("screen_width_m", "Screen width", 1000.0, 0.1, " mm"),
            Field("screen_height_m", "Screen height", 1000.0, 0.1, " mm"),
            Field("k1", "Lens curve 1", 1.0, 0.01, ""),
            Field("k2", "Lens curve 2", 1.0, 0.01, ""),
            Field("vertical_offset_m", "Lens height on screen", 1000.0, 0.1, " mm"),
            Field("fov_left_deg", "Outer view limit", 1.0, 1.0, "\u00b0"),
            Field("fov_right_deg", "Inner view limit", 1.0, 1.0, "\u00b0"),
            Field("fov_bottom_deg", "Bottom view limit", 1.0, 1.0, "\u00b0"),
            Field("fov_top_deg", "Top view limit", 1.0, 1.0, "\u00b0")
        )
        val body = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(24), dp(8), dp(24), dp(8))
        }
        body.addView(ui.label("Drag each slider until the picture looks sharp and straight. " +
            "You don't need to know the numbers.", 14f, Ui.MUTED).apply { setPadding(0, 0, 0, dp(12)) })
        val sliders = linkedMapOf<Field, SeekBar>()
        val error = TextView(this).apply { setTextColor(Ui.BAD) }
        val more = ui.column().apply { visibility = View.GONE }
        fun low(field: Field, section: String) = catalog.getJSONObject("schema").getJSONObject("properties")
            .getJSONObject(section).getJSONObject("properties").getJSONObject(field.key).getDouble("minimum") * field.scale
        fun high(field: Field, section: String) = catalog.getJSONObject("schema").getJSONObject("properties")
            .getJSONObject(section).getJSONObject("properties").getJSONObject(field.key).getDouble("maximum") * field.scale
        fun section(field: Field) = if (field.key == "ipd_mm" || field.key == "fov_scale") "display" else "lens"
        fun position(field: Field, source: JSONObject) =
            Math.round((source.getDouble(field.key) * field.scale - low(field, section(field))) / field.step).toInt()
        val reset = button("Reset to standard") {
            for ((field, bar) in sliders) {
                values.put(field.key, defaults.get(field.key))
                bar.progress = position(field, defaults)
            }
            error.text = "Standard values loaded. Save to use them."
        }.apply { tag = "viewer_generic" }
        for (field in fields) {
            val bottom = low(field, section(field))
            val steps = Math.round((high(field, section(field)) - bottom) / field.step).toInt()
            val digits = if (field.step >= 1) 0 else if (field.step >= 0.1) 1 else 2
            val at = { step: Int -> bottom + step * field.step }
            val view = ui.slider(field.label, steps, position(field, values), "viewer_${field.key}",
                { step -> java.math.BigDecimal.valueOf(at(step)).setScale(digits, java.math.RoundingMode.HALF_UP).toPlainString() + field.unit }) { step ->
                values.put(field.key, java.math.BigDecimal.valueOf(at(step)).setScale(digits + 1, java.math.RoundingMode.HALF_UP).toDouble() / field.scale)
                error.text = ""
            }
            sliders[field] = view.findViewWithTag("viewer_${field.key}")
            (if (field.main) body else more).addView(view)
        }
        body.addView(ui.quiet("More lens settings") {
            more.visibility = if (more.visibility == View.VISIBLE) View.GONE else View.VISIBLE
        }.apply { tag = "viewer_more" })
        body.addView(more)
        body.addView(reset)
        body.addView(error)
        val scroll = object : ScrollView(this) {
            override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
                val cap = (resources.displayMetrics.heightPixels * 0.5).toInt()
                val available = if (View.MeasureSpec.getMode(heightMeasureSpec) == View.MeasureSpec.UNSPECIFIED)
                    cap else minOf(cap, View.MeasureSpec.getSize(heightMeasureSpec))
                super.onMeasure(widthMeasureSpec, View.MeasureSpec.makeMeasureSpec(available, View.MeasureSpec.AT_MOST))
            }
        }.apply { addView(body) }
        val dialog = AlertDialog.Builder(this).setTitle("Adjust lenses")
            .setView(scroll)
            .setNegativeButton("Cancel", null)
            .setPositiveButton("Save", null).create()
        dialog.setOnShowListener {
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                try {
                    val saved = viewerProfiles.save(values.toString())
                    viewerValues = JSONObject(NativeBridge.viewer(nativeHandle, saved))
                    viewerSummary.text = "Saved on phone; waiting for PC"
                    dialog.dismiss()
                } catch (failure: Exception) {
                    error.text = failure.message ?: "Check the calibration values"
                }
            }
        }
        viewerDialog = dialog
        dialog.setOnDismissListener { viewerDialog = null }
        dialog.window?.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        dialog.show()
    }
    @Suppress("DEPRECATION")
    private fun scanQr(request: Int) {
        desiredConnection = false
        if (nativeHandle != 0L) NativeBridge.stop(nativeHandle)
        power(false)
        startActivityForResult(Intent(this, QrScanActivity::class.java), request)
    }
    @Suppress("DEPRECATION")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode !in setOf(4101, 4102) || resultCode != RESULT_OK) return
        val content = data?.getStringExtra("qr_text") ?: return
        try {
            if (requestCode == 4101) {
                val pc = SetupQr.connection(content)
                AlertDialog.Builder(this).setTitle("Use this PC address?")
                    .setMessage("${pc.ip}:${pc.port}")
                    .setNegativeButton("Cancel", null).setPositiveButton("Use address") { _, _ ->
                        autoSwitch.isChecked = false
                        usbSwitch.isChecked = false
                        address.setText(pc.ip); port.setText(pc.port.toString())
                        say("PC address saved. Tap Connect.")
                    }.show()
            } else importViewer(SetupQr.viewer(content))
        } catch (failure: Exception) {
            AlertDialog.Builder(this).setTitle("QR could not be imported")
                .setMessage(failure.message ?: "Invalid QR code").setPositiveButton("OK", null).show()
        }
    }
    private fun importViewer(profile: SetupQr.Viewer) {
        check(nativeHandle != 0L && ::viewerProfiles.isInitialized) { "Viewer storage is unavailable" }
        val base = viewerValues ?: JSONObject(NativeBridge.viewerCatalog()).getJSONObject("values")
        val body = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(20), dp(8), dp(20), dp(8)) }
        body.addView(TextView(this).apply {
            text = "${profile.name}\nLens spacing ${"%.1f".format(profile.spacing * 1000)} mm; screen distance ${"%.1f".format(profile.distance * 1000)} mm.\n" +
                "Confirm active screen measurements in landscape. For a top/bottom tray, measure the bezel between the tray edge and active display. Your IPD is preserved."
        })
        fun measurement(label: String, value: Double): EditText {
            body.addView(TextView(this).apply { text = label })
            return EditText(this).apply {
                setSingleLine(true); inputType = android.text.InputType.TYPE_CLASS_NUMBER or android.text.InputType.TYPE_NUMBER_FLAG_DECIMAL
                setText("%.2f".format(java.util.Locale.ROOT, value)); body.addView(this)
            }
        }
        val width = measurement("Active screen width (mm)", base.getDouble("screen_width_m") * 1000)
        val height = measurement("Active screen height (mm)", base.getDouble("screen_height_m") * 1000)
        val border = measurement("Tray edge to active screen bezel (mm); ignored for centered viewers", 3.0)
        val error = TextView(this).apply { setTextColor(Color.RED) }; body.addView(error)
        val dialog = AlertDialog.Builder(this).setTitle("Review Cardboard profile")
            .setView(ScrollView(this).apply { addView(body) }).setNegativeButton("Cancel", null)
            .setPositiveButton("Save profile", null).create()
        dialog.setOnShowListener {
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                try {
                    val candidate = profile.calibration(base, width.text.toString().toDouble(), height.text.toString().toDouble(), border.text.toString().toDouble())
                    val saved = viewerProfiles.save(candidate.toString())
                    viewerValues = JSONObject(NativeBridge.viewer(nativeHandle, saved))
                    viewerSummary.text = "${profile.name} saved on phone; waiting for PC"
                    dialog.dismiss()
                } catch (failure: Exception) { error.text = failure.message ?: "Check measurements" }
            }
        }
        viewerDialog = dialog
        dialog.setOnDismissListener { viewerDialog = null }
        dialog.show()
    }
    private fun connect() {
        if (automatic) {
            attemptAt = android.os.SystemClock.elapsedRealtime()
            handler.postDelayed({ if (resumed && desiredConnection && automatic && attempted.isEmpty()) selectTransport() }, 1500)
            return
        }
        routeLabel = if (usbAdb) "adb USB" else "Manual network"
        connectEndpoint(address.text.toString().trim(), port.text.toString().toIntOrNull() ?: 0, "")
    }
    private fun selectTransport() {
        if (nativeHandle == 0L || !::store.isInitialized || !desiredConnection) return
        if (JSONObject(NativeBridge.status(nativeHandle)).optBoolean("connected")) return
        val now = android.os.SystemClock.elapsedRealtime()
        val savedIp = address.text.toString().trim().takeIf { it != "127.0.0.1" && TransportSelection.ipv4(it) != null }
            ?: getPreferences(MODE_PRIVATE).getString("wifi_pc", "").orEmpty()
            .ifEmpty { getPreferences(MODE_PRIVATE).getString("pc", "").orEmpty() }
        if (selectedPin.isEmpty()) selectedPin = store.peer(savedIp).ifEmpty { store.peer("127.0.0.1") }
        val subnets = TransportSelection.tetherSubnets()
        val candidates = discovered.values.filter { now - it.seen < 12000 }
        val eligible = if (selectedPin.isNotEmpty()) candidates.filter { it.identity == selectedPin }
            else candidates.takeIf { it.map { pc -> pc.identity.ifEmpty { pc.ip } }.distinct().size == 1 }.orEmpty()
        if (attempted.isEmpty() && now - attemptAt < 3000 &&
            eligible.none { it.tether || TransportSelection.onTether(it.ip, subnets) }) {
            handler.postDelayed({ if (resumed && desiredConnection && automatic && attempted.isEmpty()) selectTransport() }, 500)
            return
        }
        val routes = TransportSelection.routes(eligible.map {
            TransportSelection.Route(it.ip, it.port, if (it.tether || TransportSelection.onTether(it.ip, subnets)) "USB tethering" else "Wi-Fi / LAN")
        }, subnets, savedIp.takeIf { TransportSelection.ipv4(it) != null }?.let {
            TransportSelection.Route(it, getPreferences(MODE_PRIVATE).getInt("port", 9943),
                if (TransportSelection.onTether(it, subnets)) "USB tethering" else "Wi-Fi / LAN")
        })
        val route = routes.firstOrNull { now - (attempted["${it.ip}:${it.port}"] ?: -60000L) >= 30000 } ?: return
        attempted["${route.ip}:${route.port}"] = now
        routeLabel = route.kind
        connectEndpoint(route.ip, route.port, selectedPin)
    }
    private fun connectEndpoint(ip: String, endpointPort: Int, expectedPin: String) {
        if (nativeHandle == 0L || !::store.isInitialized) return
        try {
            check(!QrScanActivity.cameraBusy) { "Setup camera is closing; tap Connect again shortly" }
            val parts = ip.split('.')
            require(parts.size == 4 && parts.all { it.isNotEmpty() && it.all(Char::isDigit) && it.toInt() in 0..255 }) { "Enter an IPv4 address" }
            val controlPort = endpointPort
            require(controlPort in 1..65535) { "Enter a port between 1 and 65535" }
            val pinned = expectedPin.ifEmpty { store.peer(ip) }
            if (!automatic) getPreferences(MODE_PRIVATE).edit().putString("pc", ip).putInt("port", controlPort)
                .also { if (!usbAdb) it.putString("wifi_pc", ip) }.apply()
            attemptAt = android.os.SystemClock.elapsedRealtime()
            lastConnected = false
            peerSaved = pinned
            connectedIp = ip
            connectedPort = controlPort
            activeTcp = if (automatic) ip == "127.0.0.1" else usbAdb
            val wired = activeTcp || TransportSelection.onTether(ip, TransportSelection.tetherSubnets())
            NativeBridge.connect(nativeHandle, ip, controlPort, pinned, true, activeTcp, wired)
        } catch (error: Exception) { say(error.message) }
    }
    private val poll = object : Runnable {
        override fun run() {
            if (!resumed) return
            if (nativeHandle != 0L) {
                try {
                    updateThermal()
                    val value = JSONObject(NativeBridge.status(nativeHandle))
                    val found = value.optJSONArray("found_pcs")
                    if (found != null) for (i in 0 until found.length()) {
                        val pc = found.getJSONObject(i)
                        remember(pc.optString("ip"), pc.optInt("port"), pc.optString("name"), pc.optString("identity"), pc.optBoolean("usb_tether"))
                    }
                    renderCandidates()
                    val connected = value.optBoolean("connected")
                    val now = android.os.SystemClock.elapsedRealtime()
                    if (automatic && desiredConnection && decoderTexture != null && !connected &&
                        (now - attemptAt > 20000 ||
                            (value.optString("error").isNotEmpty() && now - attemptAt > 2500))) {
                        selectTransport()
                    }
                    lastConnected = connected
                    val viewerSync = value.optString("viewer_sync")
                    if (viewerSync.isNotEmpty()) viewerSummary.text = viewerSync
                    status.text = value.optString("error").ifEmpty { value.optString("state", "Waiting") }
                    if (routeLabel.isNotEmpty()) status.append("\nConnection: $routeLabel · $connectedIp")
                    val mappingStatus = value.optString("mapping_status")
                    if (mappingStatus.isNotEmpty()) status.append("\n$mappingStatus")
                    val sensors = value.optJSONObject("sensors")
                    val decoder = value.optJSONObject("decoder")
                    val renderer = value.optJSONObject("renderer")
                    if (renderer != null) {
                        val rendererError = renderer.optString("error")
                        if (rendererError.isNotEmpty()) status.append("\nRenderer: $rendererError")
                    }
                    if (connected && decoder != null) {
                        value.optJSONObject("stream")?.let { stream ->
                            status.append("\nStream: ${stream.optInt("width")}×${stream.optInt("height")} · ${stream.optInt("fps")} fps")
                        }
                        if (value.optBoolean("format_changing")) status.append("\nAdjusting video quality…")
                        val decoderError = decoder.optString("error")
                        status.append(if (decoderError.isNotEmpty()) "\nVideo: $decoderError"
                            else "\nVideo: ${decoder.optLong("decoded")} decoded · p50 ${"%.1f".format(decoder.optDouble("decode_p50_ms"))} / p95 ${"%.1f".format(decoder.optDouble("decode_p95_ms"))} ms (${decoder.optInt("decode_samples")} samples)")
                        status.append(if (value.optBoolean("phone_stats_supported"))
                            "\nPC diagnostics: ${value.optLong("phone_stats_reports")} reports sent"
                            else "\nPC diagnostics: update the PC driver to receive phone stats")
                        value.optJSONObject("quality")?.let { quality ->
                            if (quality.optJSONObject("format")?.optBoolean("blocked") == true)
                                status.append("\nKeeping the previous video format after a failed change")
                            quality.optJSONObject("guard")?.let { guard ->
                                status.append("\nVideo guard: ${guard.optInt("input_fps_cap")} fps · ${guard.optString("reason")}")
                            }
                            quality.optJSONObject("fec")?.let { fec ->
                                status.append(if (fec.optBoolean("tcp_bypass")) "\nFEC: TCP bypass"
                                    else "\nFEC: ${fec.optInt("requested_ratio_pct")}% requested · ${if (fec.optBoolean("adaptive")) "automatic" else "fixed"}")
                            }
                            val accepted = quality.optLong("encoder_accepted_bitrate_bps")
                            val requested = quality.optLong("requested_bitrate_bps")
                            val mode = if (quality.optBoolean("enabled")) "Adaptive" else "Fixed"
                            status.append("\n$mode bitrate: ${"%.1f".format(accepted / 1_000_000.0)} Mbps encoder setting")
                            if (quality.optBoolean("bitrate_rejected"))
                                status.append(" · encoder rejected live updates")
                            else if (requested != accepted) status.append(" · update pending")
                        }
                    }
                    if (connected && sensors != null) {
                        val sensorError = sensors.optString("error")
                        status.append(when {
                            sensorError.isNotEmpty() -> "\nTracking: $sensorError"
                            sensors.optBoolean("calibrating") -> "\nHold the phone still for gyro calibration"
                            else -> "\nTracking: ${sensors.optDouble("rate_hz").toInt()} Hz · ${value.optLong("poses_sent")} poses sent"
                        })
                    }
                    val identity = value.optString("peer_identity")
                    if (connected && identity.isNotEmpty()) {
                        if (store.peer(connectedIp) != identity) store.trust(connectedIp, identity)
                        peerSaved = identity
                        selectedPin = identity
                    }
                    adoptFromPc(value)
                    showState(value, connected)
                    keepAwake = value.optJSONObject("settings")?.optJSONObject("power")?.optBoolean("keep_awake", true) ?: true
                    updateRotation()
                    power(connected)
                } catch (error: Exception) { status.text = "Client error: ${error.message}" }
            }
            handler.postDelayed(this, 500)
        }
    }
    private fun remember(ip: String, controlPort: Int, name: String, identity: String = "", tether: Boolean = false) {
        if (ip.isEmpty() || controlPort !in 1..65535) return
        val key = "$ip:$controlPort"
        if (key !in discovered && discovered.size >= 16) return
        discovered[key] = Candidate(ip, controlPort, name.take(96), android.os.SystemClock.elapsedRealtime(), identity.ifEmpty { discovered[key]?.identity.orEmpty() }, tether || discovered[key]?.tether == true)
    }
    private fun renderCandidates() {
        val now = android.os.SystemClock.elapsedRealtime()
        discovered.entries.removeAll { now - it.value.seen > 12000 }
        val list = discovered.keys.joinToString()
        if (list == deviceList) return
        deviceList = list
        devices.removeAllViews()
        for (pc in discovered.values) devices.addView(ui.row(pc.name.ifEmpty { "PC" }, pc.ip) {
            autoSwitch.isChecked = false
            usbSwitch.isChecked = false
            address.setText(pc.ip); port.setText(pc.port.toString())
            desiredConnection = true; attempted.clear(); selectedPin = ""; connect()
        })
    }
    @Suppress("DEPRECATION") // API29 high-performance Wi-Fi lock remains the fallback.
    private fun power(enabled: Boolean) {
        if (enabled) {
            if (keepAwake) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        }
        if (enabled && wakeLock == null) {
            previousBrightness = window.attributes.screenBrightness
            val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "PocketVR:stream").apply { acquire() }
            val wifi = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
            if (!activeTcp && routeLabel != "USB tethering") wifiLock = try { wifi.createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "PocketVR:stream").apply { acquire() } }
                catch (_: RuntimeException) { wifi.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "PocketVR:stream").apply { acquire() } }
            if (keepAwake) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            window.setSustainedPerformanceMode(true)
            window.attributes = window.attributes.apply { screenBrightness = 0.8f }
        } else if (!enabled && wakeLock != null) {
            wifiLock?.let { if (it.isHeld) it.release() }; wifiLock = null
            wakeLock?.let { if (it.isHeld) it.release() }; wakeLock = null
            if (!headset) window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            window.setSustainedPerformanceMode(false)
            window.attributes = window.attributes.apply { screenBrightness = previousBrightness }
        }
    }
    private val discovery = object : NsdManager.DiscoveryListener {
        override fun onDiscoveryStarted(type: String) {
            runOnUiThread {
                discovering = true
                if (!resumed) stopDiscovery()
            }
        }
        override fun onDiscoveryStopped(type: String) { discovering = false }
        override fun onStartDiscoveryFailed(type: String, error: Int) { discovering = false }
        override fun onStopDiscoveryFailed(type: String, error: Int) { discovering = false }
        override fun onServiceLost(service: NsdServiceInfo) {}
        @Suppress("DEPRECATION")
        override fun onServiceFound(service: NsdServiceInfo) {
            if (!service.serviceType.contains("_pocketvr._tcp")) return
            nsd.resolveService(service, object : NsdManager.ResolveListener {
                override fun onResolveFailed(service: NsdServiceInfo, error: Int) {}
                override fun onServiceResolved(info: NsdServiceInfo) {
                    val ip = (info.host as? Inet4Address)?.hostAddress ?: return
                    runOnUiThread {
                        if (resumed) { remember(ip, info.port, info.serviceName); renderCandidates() }
                    }
                }
            })
        }
    }
    override fun onResume() {
        super.onResume(); resumed = true
        phoneControls.start()
        handler.post(poll)
        if (nativeHandle != 0L) NativeBridge.discover(nativeHandle, true)
        updateRotation()
        try { nsd.discoverServices("_pocketvr._tcp.", NsdManager.PROTOCOL_DNS_SD, discovery) }
        catch (_: RuntimeException) { discovering = false }
        attachSurface()
    }
    override fun onPause() {
        resumed = false
        phoneControls.stop()
        handler.removeCallbacks(poll)
        stopDiscovery()
        if (nativeHandle != 0L) {
            NativeBridge.discover(nativeHandle, false)
            NativeBridge.stop(nativeHandle)
        }
        detachSurface()
        power(false)
        super.onPause()
    }
    private fun stopDiscovery() {
        if (discovering) try { nsd.stopServiceDiscovery(discovery) } catch (_: RuntimeException) {}
    }
    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (!hasFocus) phoneControls.release()
    }
    override fun dispatchKeyEvent(event: android.view.KeyEvent): Boolean = phoneControls.key(event) || super.dispatchKeyEvent(event)
    override fun dispatchGenericMotionEvent(event: android.view.MotionEvent): Boolean = phoneControls.motion(event) || super.dispatchGenericMotionEvent(event)
    override fun onDestroy() {
        viewerDialog?.dismiss()
        if (nativeHandle != 0L) NativeBridge.destroy(nativeHandle)
        nativeHandle = 0
        (getSystemService(DISPLAY_SERVICE) as android.hardware.display.DisplayManager)
            .unregisterDisplayListener(displayListener)
        super.onDestroy()
    }
    // A flip between the two landscape directions is not a configuration change, so
    // onConfigurationChanged never hears about it.
    private val displayListener = object : android.hardware.display.DisplayManager.DisplayListener {
        override fun onDisplayAdded(displayId: Int) {}
        override fun onDisplayRemoved(displayId: Int) {}
        override fun onDisplayChanged(displayId: Int) = updateRotation()
    }
    @Suppress("DEPRECATION")
    private fun updateRotation() {
        if (nativeHandle != 0L) {
            val current = window.decorView.display ?: windowManager.defaultDisplay
            NativeBridge.rotation(nativeHandle, current.rotation)
            NativeBridge.displayRate(nativeHandle, current.refreshRate.toDouble())
        }
    }
    private fun updateThermal() {
        if (nativeHandle == 0L) return
        val level = try { (getSystemService(Context.POWER_SERVICE) as PowerManager).currentThermalStatus }
            catch (_: RuntimeException) { -1 }
        NativeBridge.thermal(nativeHandle, level)
    }
    override fun onConfigurationChanged(configuration: android.content.res.Configuration) {
        super.onConfigurationChanged(configuration)
        updateRotation()
    }
    private fun capabilities(): JSONObject {
        val codecs = JSONArray()
        for (codec in MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos) {
            if (codec.isEncoder || !codec.isHardwareAccelerated) continue
            for (type in codec.supportedTypes) {
                if (type != "video/avc" && type != "video/hevc") continue
                try {
                    val video = codec.getCapabilitiesForType(type).videoCapabilities ?: continue
                    val sizes = JSONArray()
                    for ((width, height) in listOf(2160 to 1080, 1920 to 960, 1836 to 918,
                            1512 to 756, 1280 to 640, 1188 to 594))
                        for (fps in listOf(30, 45, 60))
                            if (video.areSizeAndRateSupported(width, height, fps.toDouble()))
                                sizes.put(JSONObject().put("width", width).put("height", height).put("fps", fps))
                    codecs.put(JSONObject().put("mime", type).put("name", codec.name).put("sizes", sizes)
                        .put("supports_2160x1080_60", video.areSizeAndRateSupported(2160, 1080, 60.0))
                        .put("max_width", video.supportedWidths.upper).put("max_height", video.supportedHeights.upper))
                } catch (_: IllegalArgumentException) {}
            }
        }
        val sensors = JSONArray()
        val manager = getSystemService(Context.SENSOR_SERVICE) as SensorManager
        for (sensor in manager.getSensorList(android.hardware.Sensor.TYPE_ALL))
            sensors.put(JSONObject().put("type", sensor.type).put("name", sensor.name).put("min_delay_us", sensor.minDelay))
        val metrics = resources.displayMetrics
        val battery = registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        return JSONObject().put("model", "${Build.MANUFACTURER} ${Build.MODEL}")
            .put("android_api", Build.VERSION.SDK_INT).put("codecs", codecs).put("sensors", sensors)
            .put("screen_width", maxOf(metrics.widthPixels, metrics.heightPixels))
            .put("screen_height", minOf(metrics.widthPixels, metrics.heightPixels))
            .put("xdpi", metrics.xdpi).put("ydpi", metrics.ydpi).put("display_hz", (window.decorView.display ?: windowManager.defaultDisplay).refreshRate)
            .put("thermal_api", Build.VERSION.SDK_INT >= 30)
            .put("battery_pct", battery?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1)
    }
}
