package dev.airscreen

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.pm.ApplicationInfo
import android.graphics.Color
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.net.ConnectivityManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.util.Log
import android.util.TypedValue
import android.view.Gravity
import android.view.KeyEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import java.io.File
import java.net.Inet4Address
import java.security.SecureRandom
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

class MainActivity : Activity() {
    private val main = Handler(Looper.getMainLooper())
    private lateinit var surface: AspectSurfaceView
    private lateinit var standby: View
    private lateinit var title: TextView
    private lateinit var subtitle: TextView
    private lateinit var hint: TextView
    private lateinit var scaleHint: TextView
    private val hideScaleHint = Runnable { scaleHint.visibility = View.GONE }

    private var landscapeScale = ScaleMode.Fill

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        applyMinimalPostProcessing(intent)
        landscapeScale = prefs().getString(PREF_LANDSCAPE_SCALE, null)
            ?.let { saved -> ScaleMode.entries.firstOrNull { it.name == saved } } ?: ScaleMode.Fill
        surface = AspectSurfaceView(this)
        surface.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                holder.surface.setFrameRate(60f, Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE)
                Native.nativeSetSurface(holder.surface)
            }
            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}
            override fun surfaceDestroyed(holder: SurfaceHolder) = Native.nativeSetSurface(null)
        })
        title = text(40f)
        subtitle = text(22f)
        hint = text(22f)
        scaleHint = text(28f).apply {
            setBackgroundColor(0xB0000000.toInt())
            setPadding(48, 24, 48, 24)
            visibility = View.GONE
        }
        standby = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            setBackgroundColor(Color.BLACK)
            addView(title)
            addView(subtitle)
            addView(hint)
        }
        setContentView(FrameLayout(this).apply {
            setBackgroundColor(Color.BLACK)
            addView(surface, FrameLayout.LayoutParams(-1, -1, Gravity.CENTER))
            addView(standby, FrameLayout.LayoutParams(-1, -1))
            addView(scaleHint, FrameLayout.LayoutParams(-2, -2, Gravity.CENTER))
        })
    }

    override fun onStart() {
        super.onStart()
        Receiver.onStart(this)
    }

    override fun onStop() {
        Receiver.onStop(this)
        super.onStop()
    }

    /** Starts the native receiver configured for this activity's display; returns its status. */
    fun startNative(): Int {
        val config = displayConfig()
        return Native.nativeStart(
            deviceName(), hwAddr(), config.width, config.height, config.refreshHz, min(config.refreshHz, 60),
            config.avcDecoder, config.hevcDecoder, File(filesDir, "airplay.pem").path, filesDir.path, replayPath(),
        )
    }

    fun render(state: ReceiverState) {
        when (val s = state) {
            ReceiverState.Starting -> showStandby("AirScreen", "正在启动…", "")
            is ReceiverState.Advertising ->
                showStandby(s.name, s.ip, "在 iPhone 控制中心点“屏幕镜像”，选择 ${s.name}")
            is ReceiverState.Mirroring -> {
                standby.visibility = View.GONE
                surface.setStream(s.width, s.height, landscapeScale)
            }
            is ReceiverState.AudioOnly -> showStandby(deviceName(), "正在播放 ${s.client} 的音频", "")
            is ReceiverState.Failed -> showStandby("AirScreen", "出错了", s.reason)
        }
        if (state is ReceiverState.Mirroring) {
            window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        } else {
            window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        }
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        val s = Receiver.state
        val confirm = keyCode == KeyEvent.KEYCODE_DPAD_CENTER || keyCode == KeyEvent.KEYCODE_ENTER
        val parent = surface.parent as View
        if (!confirm || s !is ReceiverState.Mirroring || !fillApplies(parent.width, parent.height, s.width, s.height)) {
            return super.onKeyDown(keyCode, event)
        }
        if (event.repeatCount == 0) toggleLandscapeScale()
        return true
    }

    private fun toggleLandscapeScale() {
        landscapeScale = if (landscapeScale == ScaleMode.Fill) ScaleMode.Fit else ScaleMode.Fill
        prefs().edit().putString(PREF_LANDSCAPE_SCALE, landscapeScale.name).apply()
        render(Receiver.state)
        scaleHint.text = if (landscapeScale == ScaleMode.Fill) "填满屏幕" else "完整画面（按确认键切换）"
        scaleHint.visibility = View.VISIBLE
        main.removeCallbacks(hideScaleHint)
        main.postDelayed(hideScaleHint, 1500)
    }

    /** Debug builds only: `am start -S -n dev.airscreen/.MainActivity --es replay <recording>`. */
    private fun replayPath(): String? =
        intent.getStringExtra("replay")?.takeIf { applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE != 0 }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        applyMinimalPostProcessing(intent)
    }

    /**
     * Asks the display for its low-latency (game) mode over HDMI ALLM. On by default; `--ez allm false` on an
     * `am start` turns it off and the choice persists, for A/B checks of how the display renders game mode.
     */
    private fun applyMinimalPostProcessing(intent: Intent?) {
        if (intent?.hasExtra(EXTRA_ALLM) == true) {
            prefs().edit().putBoolean(PREF_ALLM, intent.getBooleanExtra(EXTRA_ALLM, true)).apply()
        }
        val allm = prefs().getBoolean(PREF_ALLM, true)
        window.attributes = window.attributes.apply { preferMinimalPostProcessing = allm }
        Log.i("AirScreen", "preferMinimalPostProcessing=$allm")
    }

    private fun prefs() = getSharedPreferences("airscreen", Context.MODE_PRIVATE)

    private fun showStandby(titleText: String, subtitleText: String, hintText: String) {
        standby.visibility = View.VISIBLE
        title.text = titleText
        subtitle.text = subtitleText
        hint.text = hintText
    }

    private fun text(sp: Float) = TextView(this).apply {
        setTextColor(Color.WHITE)
        setTextSize(TypedValue.COMPLEX_UNIT_SP, sp)
        gravity = Gravity.CENTER
        setPadding(0, 12, 0, 12)
    }

    fun deviceName(): String =
        Settings.Global.getString(contentResolver, "device_name")?.takeIf { it.isNotBlank() } ?: Build.MODEL

    fun ipAddress(): String {
        val cm = getSystemService(ConnectivityManager::class.java)
        return cm.getLinkProperties(cm.activeNetwork)?.linkAddresses
            ?.map { it.address }?.firstOrNull { it is Inet4Address }?.hostAddress ?: "未连接网络"
    }

    /** Random, persisted, locally administered unicast MAC-like id. */
    private fun hwAddr(): ByteArray {
        val file = File(filesDir, "hw_addr")
        if (file.exists() && file.length() == 6L) return file.readBytes()
        val addr = ByteArray(6).also { SecureRandom().nextBytes(it) }
        addr[0] = ((addr[0].toInt() or 0x02) and 0xfe).toByte()
        file.writeBytes(addr)
        return addr
    }

    private data class DisplayConfig(
        val width: Int, val height: Int, val refreshHz: Int, val avcDecoder: String?, val hevcDecoder: String?,
    )

    private fun displayConfig(): DisplayConfig {
        val mode = display!!.mode
        val native = Size(max(mode.physicalWidth, mode.physicalHeight), min(mode.physicalWidth, mode.physicalHeight))
        val size = if (MIMES.any { hardwareDecoder(it, native) != null }) native else Size(1920, 1080)
        val (avc, hevc) = MIMES.map { hardwareDecoder(it, size) }
        Log.i(TAG, "video decoders: avc $avc, hevc $hevc")
        return DisplayConfig(size.width, size.height, mode.refreshRate.roundToInt(), avc, hevc)
    }

    /** The hardware decoder for [mime] at [size], preferring one that declares FEATURE_LowLatency. */
    private fun hardwareDecoder(mime: String, size: Size): String? {
        val decoders = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.filter { info ->
            !info.isEncoder && info.isHardwareAccelerated && mime in info.supportedTypes &&
                info.getCapabilitiesForType(mime).videoCapabilities.isSizeSupported(size.width, size.height)
        }
        val lowLatency = decoders.firstOrNull {
            it.getCapabilitiesForType(mime).isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency)
        }
        return (lowLatency ?: decoders.firstOrNull())?.name
    }

    private companion object {
        const val TAG = "AirScreen"
        const val PREF_LANDSCAPE_SCALE = "landscape_scale"
        const val PREF_ALLM = "allm"
        const val EXTRA_ALLM = "allm"
        val MIMES = listOf("video/avc", "video/hevc")
    }
}

/** SurfaceView sized to the stream per [streamViewSize], centered by its parent; Fill overflow is clipped. */
class AspectSurfaceView(context: Context) : SurfaceView(context) {
    private var streamWidth = 0
    private var streamHeight = 0
    private var landscape = ScaleMode.Fit

    fun setStream(width: Int, height: Int, landscape: ScaleMode) {
        if (width != streamWidth || height != streamHeight || landscape != this.landscape) {
            streamWidth = width
            streamHeight = height
            this.landscape = landscape
            requestLayout()
        }
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val size = streamViewSize(
            MeasureSpec.getSize(widthMeasureSpec), MeasureSpec.getSize(heightMeasureSpec),
            streamWidth, streamHeight, landscape,
        )
        setMeasuredDimension(size.width, size.height)
    }
}
