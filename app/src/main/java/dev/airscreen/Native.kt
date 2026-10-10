package dev.airscreen

import android.view.Surface

object Native {
    interface Sink {
        /** Called on native threads. */
        fun onEvent(event: NativeEvent)
        fun registerService(service: Int, name: String, port: Int, txt: ByteArray)
        fun unregisterService(service: Int)
    }

    /** DNSSD_SERVICE_RAOP / DNSSD_SERVICE_AIRPLAY in dnssd.h. */
    const val SERVICE_RAOP = 0
    const val SERVICE_AIRPLAY = 1

    @Volatile
    var sink: Sink? = null

    init {
        System.loadLibrary("airscreen")
    }

    @JvmStatic
    external fun nativeStart(
        name: String, hwAddr: ByteArray, width: Int, height: Int,
        refreshHz: Int, maxFps: Int, h265: Boolean, keyfile: String,
    ): Int

    @JvmStatic
    external fun nativeStop()

    @JvmStatic
    fun onEvent(kind: Int, text: String?, a: Int, b: Int) {
        sink?.onEvent(NativeEvent.fromNative(kind, text, a, b))
    }

    @JvmStatic
    fun registerService(service: Int, name: String, port: Int, txt: ByteArray) {
        sink?.registerService(service, name, port, txt)
    }

    @JvmStatic
    fun unregisterService(service: Int) {
        sink?.unregisterService(service)
    }
}
