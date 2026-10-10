package dev.airscreen

import android.view.Surface

object Native {
    /** DNSSD_SERVICE_RAOP / DNSSD_SERVICE_AIRPLAY in dnssd.h. */
    const val SERVICE_RAOP = 0
    const val SERVICE_AIRPLAY = 1

    init {
        System.loadLibrary("airscreen")
    }

    /**
     * Decoder names are MediaCodec codec names, null when there is none; HEVC is advertised only with one.
     * With [replay] set, plays that session recording through the renderers instead of serving AirPlay.
     */
    @JvmStatic
    external fun nativeStart(
        name: String, hwAddr: ByteArray, width: Int, height: Int, refreshHz: Int, maxFps: Int,
        avcDecoder: String?, hevcDecoder: String?, keyfile: String, filesDir: String, replay: String?,
    ): Int

    @JvmStatic
    external fun nativeStop()

    /** Null when the surface is destroyed; must return before surfaceDestroyed does. */
    @JvmStatic
    external fun nativeSetSurface(surface: Surface?)

    /** Called on native threads. */
    @JvmStatic
    fun onEvent(kind: Int, text: String?, a: Int, b: Int) {
        Receiver.post(NativeEvent.fromNative(kind, text, a, b))
    }

    @JvmStatic
    fun registerService(service: Int, name: String, port: Int, txt: ByteArray) {
        Receiver.registerService(service, name, port, txt)
    }

    @JvmStatic
    fun unregisterService(service: Int) {
        Receiver.unregisterService(service)
    }
}
