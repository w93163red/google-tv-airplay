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

    private var state: ReceiverState = ReceiverState.Starting
    private var idle = ReceiverState.Advertising("", "")

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        advertiser = ServiceAdvertiser(this)
        surface = AspectSurfaceView(this)
        title = text(40f)
        subtitle = text(22f)
        hint = text(22f)
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
                surface.setAspect(s.width, s.height)
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
}

/** SurfaceView letterboxed to the stream's aspect ratio inside its parent. */
class AspectSurfaceView(context: Context) : SurfaceView(context) {
    private var aspect = 0f

    fun setAspect(width: Int, height: Int) {
        val next = if (width > 0 && height > 0) width.toFloat() / height else 0f
        if (next != aspect) {
            aspect = next
            requestLayout()
        }
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val w = MeasureSpec.getSize(widthMeasureSpec)
        val h = MeasureSpec.getSize(heightMeasureSpec)
        if (aspect <= 0f || w == 0 || h == 0) {
            setMeasuredDimension(w, h)
        } else if (w > h * aspect) {
            setMeasuredDimension((h * aspect).roundToInt(), h)
        } else {
            setMeasuredDimension(w, (w / aspect).roundToInt())
        }
    }
}
