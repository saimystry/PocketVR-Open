// SPDX-License-Identifier: Apache-2.0
package com.pocketvr

import android.app.AlertDialog
import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.RippleDrawable
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.SeekBar
import android.widget.Switch
import android.widget.TextView

/** The app's small design system: one palette and a few reusable building blocks. */
class Ui(private val context: Context) {
    companion object {
        val BACKGROUND = Color.rgb(11, 15, 23)
        val SURFACE = Color.rgb(20, 26, 40)
        val RAISED = Color.rgb(31, 39, 58)
        val LINE = Color.rgb(42, 51, 72)
        val TEXT = Color.rgb(238, 242, 248)
        val MUTED = Color.rgb(146, 157, 176)
        val ACCENT = Color.rgb(108, 140, 255)
        val GOOD = Color.rgb(52, 211, 153)
        val WARN = Color.rgb(251, 191, 36)
        val BAD = Color.rgb(248, 113, 113)
    }
    fun dp(value: Int) = (value * context.resources.displayMetrics.density).toInt()
    private fun shape(color: Int, radius: Int, line: Int = 0) = GradientDrawable().apply {
        setColor(color); cornerRadius = dp(radius).toFloat()
        if (line != 0) setStroke(dp(1), line)
    }
    private fun pressable(color: Int, radius: Int, line: Int = 0) =
        RippleDrawable(ColorStateList.valueOf(Color.argb(50, 255, 255, 255)), shape(color, radius, line), shape(Color.WHITE, radius))
    private fun <T : View> spaced(view: T, bottom: Int = 0): T = view.apply {
        layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            bottomMargin = dp(bottom)
        }
    }
    fun label(value: String, size: Float = 15f, color: Int = TEXT, bold: Boolean = false) = TextView(context).apply {
        text = value; textSize = size; setTextColor(color)
        if (bold) typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
        setLineSpacing(0f, 1.15f)
    }
    fun column(padding: Int = 0) = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL; setPadding(dp(padding), dp(padding), dp(padding), dp(padding))
    }
    fun card(vararg children: View) = spaced(column(16).apply {
        background = shape(SURFACE, 18, LINE)
        children.forEach { addView(it) }
    }, 12)
    private fun styled(value: String, color: Int, textColor: Int, line: Int, action: () -> Unit) = spaced(Button(context).apply {
        text = value; isAllCaps = false; textSize = 16f; stateListAnimator = null
        typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
        setTextColor(textColor); background = pressable(color, 14, line)
        minHeight = dp(54); setOnClickListener { action() }
    }, 10)
    fun primary(value: String, action: () -> Unit) = styled(value, ACCENT, Color.rgb(9, 13, 24), 0, action)
    fun secondary(value: String, action: () -> Unit) = styled(value, RAISED, TEXT, 0, action)
    fun quiet(value: String, action: () -> Unit) = styled(value, Color.TRANSPARENT, MUTED, LINE, action)
    /** Two equal buttons on one line. */
    fun pair(first: View, second: View) = LinearLayout(context).apply {
        orientation = LinearLayout.HORIZONTAL
        val gap = dp(5)
        addView(first, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply { marginEnd = gap; bottomMargin = dp(10) })
        addView(second, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply { marginStart = gap; bottomMargin = dp(10) })
    }
    fun section(value: String) = label(value.uppercase(), 12f, MUTED, true).apply {
        letterSpacing = 0.08f; setPadding(dp(4), dp(14), 0, dp(8))
    }
    private fun line(title: String, detail: String?, trailing: View?) = LinearLayout(context).apply {
        orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL
        minimumHeight = dp(52); setPadding(0, dp(8), 0, dp(8))
        val words = column().apply {
            addView(label(title, 15f))
            if (detail != null) addView(label(detail, 13f, MUTED).apply { tag = "detail" })
        }
        addView(words, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        if (trailing != null) addView(trailing)
    }
    /** A settings line that opens something. */
    fun row(title: String, detail: String? = null, action: () -> Unit) =
        line(title, detail, label("›", 22f, MUTED)).apply {
            isClickable = true; setOnClickListener { action() }
            foreground = RippleDrawable(ColorStateList.valueOf(Color.argb(40, 255, 255, 255)), null, shape(Color.WHITE, 10))
        }
    /** A settings line with an on/off switch. */
    fun toggle(title: String, detail: String?, switch: Switch) = line(title, detail, switch.apply {
        text = ""
        thumbTintList = ColorStateList(arrayOf(intArrayOf(android.R.attr.state_checked), intArrayOf()), intArrayOf(ACCENT, MUTED))
    })
    /** A settings line that picks one option from a short list. */
    fun choice(title: String, options: Array<String>, current: Int, pick: (Int) -> Unit): View {
        var selected = current.coerceIn(0, options.lastIndex)
        lateinit var view: LinearLayout
        view = row(title, options[selected]) {
            AlertDialog.Builder(context).setTitle(title).setSingleChoiceItems(options, selected) { dialog, index ->
                selected = index
                view.findViewWithTag<TextView>("detail").text = options[index]
                pick(index); dialog.dismiss()
            }.setNegativeButton("Cancel", null).show()
        }
        return view
    }
    /** A slider with its current value, plus - and + buttons for one small step. */
    fun slider(title: String, steps: Int, current: Int, name: String, format: (Int) -> String, change: (Int) -> Unit): View {
        val value = label(format(current), 15f, ACCENT, true)
        val heading = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(label(title, 15f), LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            addView(value)
        }
        val bar = SeekBar(context).apply {
            tag = name; max = steps; progress = current.coerceIn(0, steps)
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(seek: SeekBar, position: Int, fromUser: Boolean) {
                    value.text = format(position)
                    if (fromUser) change(position)
                }
                override fun onStartTrackingTouch(seek: SeekBar) {}
                override fun onStopTrackingTouch(seek: SeekBar) {}
            })
        }
        fun nudge(symbol: String, by: Int) = label(symbol, 24f, TEXT, true).apply {
            gravity = Gravity.CENTER; minWidth = dp(44); minHeight = dp(44); isClickable = true
            setOnClickListener {
                val next = (bar.progress + by).coerceIn(0, steps)
                if (next != bar.progress) { bar.progress = next; change(next) }
            }
        }
        val controls = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL
            addView(nudge("\u2212", -1))
            addView(bar, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            addView(nudge("+", 1))
        }
        return spaced(column().apply { addView(heading); addView(controls) }, 8)
    }
    fun field(hintText: String, kind: Int, value: String, name: String) = spaced(EditText(context).apply {
        tag = name; hint = hintText; inputType = kind; setSingleLine(true); setText(value)
        textSize = 16f; setTextColor(TEXT); setHintTextColor(MUTED)
        background = shape(RAISED, 12); setPadding(dp(14), dp(12), dp(14), dp(12)); minHeight = dp(48)
    }, 8)
    fun dot() = View(context).apply {
        layoutParams = LinearLayout.LayoutParams(dp(10), dp(10)).apply { marginEnd = dp(10) }
        background = GradientDrawable().apply { shape = GradientDrawable.OVAL; setColor(MUTED) }
    }
    fun tint(dot: View, color: Int) { (dot.background as GradientDrawable).setColor(color) }
    fun hero() = column().apply {
        gravity = Gravity.CENTER
        background = GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, intArrayOf(Color.rgb(30, 40, 74), BACKGROUND))
        addView(ImageView(context).apply {
            setImageResource(R.drawable.ic_launcher)
            clipToOutline = true; background = shape(SURFACE, 20)
        }, LinearLayout.LayoutParams(dp(76), dp(76)).apply { bottomMargin = dp(10) })
        addView(label("PocketVR", 26f, TEXT, true).apply { gravity = Gravity.CENTER })
    }
}
