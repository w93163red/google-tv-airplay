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
    private lateinit var surface: StreamSurfaceView
    private lateinit var sampler: ContentSampler
    private lateinit var standby: View
    private lateinit var title: TextView
    private lateinit var subtitle: TextView
    private lateinit var hint: TextView
    private lateinit var zoomHint: TextView
    private val hideZoomHint = Runnable { zoomHint.visibility = View.GONE }

    private var zoomMode = ZoomMode.Smart
    private var zoom: ZoomState = ZoomState.Idle

    /** The stream [sampler] watches for bars, or null while it is stopped. */
    private var sampled: Size? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        applyMinimalPostProcessing(intent)
        zoomMode = prefs().getString(PREF_ZOOM_MODE, null)
            ?.let { saved -> ZoomMode.entries.firstOrNull { it.name == saved } } ?: ZoomMode.Smart
        surface = StreamSurfaceView(this)
        sampler = ContentSampler(surface, ::onSample)
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
        zoomHint = text(28f).apply {
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
            addView(zoomHint, FrameLayout.LayoutParams(-2, -2, Gravity.CENTER))
        })
    }

    override fun onStart() {
        super.onStart()
        Receiver.onStart(this)
    }

    override fun onStop() {
        // Receiver renders only to started activities, so this one would never see the session end.
        sampleForBars(null)
        Receiver.onStop(this)
        super.onStop()
    }

    override fun onDestroy() {
        sampler.quit()
        super.onDestroy()
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
        val stream = (state as? ReceiverState.Mirroring)?.let { Size(it.width, it.height) }
        sampleForBars(stream?.takeIf { zoomMode == ZoomMode.Smart && it.width > 0 && it.height > 0 })
        when (val s = state) {
            ReceiverState.Starting -> showStandby("AirScreen", "正在启动…", "")
            is ReceiverState.Advertising ->
                showStandby(s.name, s.ip, "在 iPhone 控制中心点“屏幕镜像”，选择 ${s.name}")
            is ReceiverState.Mirroring -> {
                standby.visibility = View.GONE
                surface.show(Size(s.width, s.height), zoomMode, zoom.zoom)
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

    /** Watches [stream] for bars, starting again from no zoom when it changes; null stops watching. */
    private fun sampleForBars(stream: Size?) {
        if (stream == sampled) return
        sampled = stream
        zoom = ZoomState.Idle
        if (stream == null) sampler.stop() else sampler.start(stream)
    }

    private fun onSample(content: ContentRect?, now: Long) {
        val next = zoom.next(content, now)
        val changed = next.zoom != zoom.zoom
        zoom = next
        if (changed) {
            Log.i(TAG, "zoom ${next.zoom ?: "off"}")
            render(Receiver.state)
        }
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        val confirm = keyCode == KeyEvent.KEYCODE_DPAD_CENTER || keyCode == KeyEvent.KEYCODE_ENTER
        if (!confirm || Receiver.state !is ReceiverState.Mirroring) return super.onKeyDown(keyCode, event)
        if (event.repeatCount == 0) toggleZoomMode()
        return true
    }

    private fun toggleZoomMode() {
        zoomMode = if (zoomMode == ZoomMode.Smart) ZoomMode.Full else ZoomMode.Smart
        prefs().edit().putString(PREF_ZOOM_MODE, zoomMode.name).apply()
        render(Receiver.state)
        zoomHint.text = if (zoomMode == ZoomMode.Smart) "智能填满" else "完整画面（按确认键切换）"
        zoomHint.visibility = View.VISIBLE
        main.removeCallbacks(hideZoomHint)
        main.postDelayed(hideZoomHint, 1500)
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
        const val PREF_ZOOM_MODE = "zoom_mode"
        const val PREF_ALLM = "allm"
        const val EXTRA_ALLM = "allm"
        val MIMES = listOf("video/avc", "video/hevc")
    }
}

/**
 * SurfaceView laid out at the size that fits the whole stream, centered by its parent. Zoom is a scale and
 * translation on top of that layout, animated, so a zoom change never resizes the surface.
 */
class StreamSurfaceView(context: Context) : SurfaceView(context) {
    private var stream = Size(0, 0)
    private var mode = ZoomMode.Smart
    private var content: ContentRect? = null
    private var parentSize = Size(0, 0)
    private var shown: Placement? = null

    fun show(stream: Size, mode: ZoomMode, content: ContentRect?) {
        val resized = stream != this.stream
        this.stream = stream
        this.mode = mode
        this.content = content
        if (resized) requestLayout() else place(animate = true)
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        parentSize = Size(MeasureSpec.getSize(widthMeasureSpec), MeasureSpec.getSize(heightMeasureSpec))
        val whole = streamPlacement(parentSize, stream, ZoomMode.Full, null)
        setMeasuredDimension(whole.width, whole.height)
    }

    override fun onLayout(changed: Boolean, left: Int, top: Int, right: Int, bottom: Int) {
        super.onLayout(changed, left, top, right, bottom)
        if (changed) shown = null
        place(animate = false)
    }

    private fun place(animate: Boolean) {
        if (width == 0 || height == 0) return
        val target = streamPlacement(parentSize, stream, mode, content)
        if (target == shown) return
        shown = target
        val scaleX = target.width.toFloat() / width
        val scaleY = target.height.toFloat() / height
        val translationX = target.x + target.width / 2f - parentSize.width / 2f
        val translationY = target.y + target.height / 2f - parentSize.height / 2f
        animate().cancel()
        if (animate) {
            animate().scaleX(scaleX).scaleY(scaleY).translationX(translationX).translationY(translationY)
                .setDuration(ZOOM_ANIMATION_MS).start()
        } else {
            this.scaleX = scaleX
            this.scaleY = scaleY
            this.translationX = translationX
            this.translationY = translationY
        }
    }

    private companion object {
        const val ZOOM_ANIMATION_MS = 250L
    }
}
