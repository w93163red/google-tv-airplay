package dev.airscreen

import android.app.Activity
import android.content.Context
import android.graphics.Color
import android.media.MediaCodecList
import android.net.ConnectivityManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.util.TypedValue
import android.view.Gravity
import android.view.KeyEvent
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

class MainActivity : Activity(), Native.Sink {
    private val main = Handler(Looper.getMainLooper())
    private lateinit var advertiser: ServiceAdvertiser
    private lateinit var surface: AspectSurfaceView
    private lateinit var standby: View
    private lateinit var title: TextView
    private lateinit var subtitle: TextView
    private lateinit var hint: TextView
    private lateinit var scaleHint: TextView
    private val hideScaleHint = Runnable { scaleHint.visibility = View.GONE }

    private var state: ReceiverState = ReceiverState.Starting
    private var idle = ReceiverState.Advertising("", "")
    private var landscapeScale = ScaleMode.Fill

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        landscapeScale = prefs().getString(PREF_LANDSCAPE_SCALE, null)
            ?.let { saved -> ScaleMode.entries.firstOrNull { it.name == saved } } ?: ScaleMode.Fill
        advertiser = ServiceAdvertiser(this)
        surface = AspectSurfaceView(this)
        surface.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) = Native.nativeSetSurface(holder.surface)
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
        render()
    }

    override fun onStart() {
        super.onStart()
        val name = deviceName()
        idle = ReceiverState.Advertising(name, ipAddress())
        Native.sink = this
        val (width, height, refresh, h265) = displayConfig()
        val result = Native.nativeStart(
            name, hwAddr(), width, height, refresh, min(refresh, 60), h265,
            File(filesDir, "airplay.pem").path,
        )
        if (result != 0) dispatch(NativeEvent.Failed("AirPlay 服务启动失败 ($result)"))
    }

    override fun onStop() {
        Native.nativeStop()
        Native.sink = null
        dispatch(NativeEvent.Disconnected)
        super.onStop()
    }

    override fun onEvent(event: NativeEvent) {
        main.post { dispatch(event) }
    }

    override fun registerService(service: Int, name: String, port: Int, txt: ByteArray) =
        advertiser.register(service, name, port, txt)

    override fun unregisterService(service: Int) = advertiser.unregister(service)

    private fun dispatch(event: NativeEvent) {
        state = reduce(state, event, idle)
        render()
    }

    private fun render() {
        when (val s = state) {
            ReceiverState.Starting -> showStandby("AirScreen", "正在启动…", "")
            is ReceiverState.Advertising ->
                showStandby(s.name, s.ip, "在 iPhone 控制中心点“屏幕镜像”，选择 ${s.name}")
            is ReceiverState.Mirroring -> {
                standby.visibility = View.GONE
                surface.setStream(s.width, s.height, landscapeScale)
            }
            is ReceiverState.AudioOnly -> showStandby(idle.name, "正在播放 ${s.client} 的音频", "")
            is ReceiverState.Failed -> showStandby("AirScreen", "出错了", s.reason)
        }
        if (state is ReceiverState.Mirroring) {
            window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        } else {
            window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        }
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        val s = state
        val confirm = keyCode == KeyEvent.KEYCODE_DPAD_CENTER || keyCode == KeyEvent.KEYCODE_ENTER
        if (!confirm || s !is ReceiverState.Mirroring || s.width <= 0 || s.height > s.width) {
            return super.onKeyDown(keyCode, event)
        }
        if (event.repeatCount == 0) toggleLandscapeScale()
        return true
    }

    private fun toggleLandscapeScale() {
        landscapeScale = if (landscapeScale == ScaleMode.Fill) ScaleMode.Fit else ScaleMode.Fill
        prefs().edit().putString(PREF_LANDSCAPE_SCALE, landscapeScale.name).apply()
        render()
        scaleHint.text = if (landscapeScale == ScaleMode.Fill) "填满屏幕" else "完整画面（按确认键切换）"
        scaleHint.visibility = View.VISIBLE
        main.removeCallbacks(hideScaleHint)
        main.postDelayed(hideScaleHint, 1500)
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

    private fun deviceName(): String =
        Settings.Global.getString(contentResolver, "device_name")?.takeIf { it.isNotBlank() } ?: Build.MODEL

    private fun ipAddress(): String {
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

    private data class DisplayConfig(val width: Int, val height: Int, val refreshHz: Int, val h265: Boolean)

    private fun displayConfig(): DisplayConfig {
        val mode = display!!.mode
        val width = max(mode.physicalWidth, mode.physicalHeight)
        val height = min(mode.physicalWidth, mode.physicalHeight)
        val refresh = mode.refreshRate.roundToInt()
        if (hardwareDecoderSupports("video/hevc", width, height)) {
            return DisplayConfig(width, height, refresh, true)
        }
        if (hardwareDecoderSupports("video/avc", width, height)) {
            return DisplayConfig(width, height, refresh, false)
        }
        return DisplayConfig(1920, 1080, refresh, false)
    }

    private fun hardwareDecoderSupports(mime: String, width: Int, height: Int): Boolean =
        MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.any { info ->
            !info.isEncoder && info.isHardwareAccelerated && info.supportedTypes.any { it == mime } &&
                info.getCapabilitiesForType(mime).videoCapabilities.isSizeSupported(width, height)
        }

    private companion object {
        const val PREF_LANDSCAPE_SCALE = "landscape_scale"
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
