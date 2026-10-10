package dev.airscreen

import android.os.Handler
import android.os.Looper

/**
 * Owns the native receiver for the process. It runs while at least one [MainActivity] is started, so an
 * activity instance starting or stopping while another is started never restarts or ends a session.
 * Main thread only, except [post].
 */
object Receiver {
    private val main = Handler(Looper.getMainLooper())
    private val started = LinkedHashSet<MainActivity>()
    private var idle = ReceiverState.Advertising("", "")
    private var advertiser: ServiceAdvertiser? = null

    var state: ReceiverState = ReceiverState.Starting
        private set

    fun onStart(activity: MainActivity) {
        started += activity
        if (started.size == 1) {
            idle = ReceiverState.Advertising(activity.deviceName(), activity.ipAddress())
            advertiser = ServiceAdvertiser(activity.applicationContext)
            val result = activity.startNative()
            if (result != 0) dispatch(NativeEvent.Failed("AirPlay 服务启动失败 ($result)"))
        }
        activity.render(state)
    }

    fun onStop(activity: MainActivity) {
        if (started.remove(activity) && started.isEmpty()) {
            Native.nativeStop()
            advertiser = null
            dispatch(NativeEvent.Disconnected)
        }
    }

    fun post(event: NativeEvent) {
        main.post { dispatch(event) }
    }

    /** Called from nativeStart and nativeStop, so on the main thread. */
    fun registerService(service: Int, name: String, port: Int, txt: ByteArray) =
        advertiser?.register(service, name, port, txt)

    fun unregisterService(service: Int) = advertiser?.unregister(service)

    private fun dispatch(event: NativeEvent) {
        state = reduce(state, event, idle)
        started.forEach { it.render(state) }
    }
}
