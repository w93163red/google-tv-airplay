package dev.airscreen

import android.graphics.Bitmap
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.view.PixelCopy
import android.view.SurfaceView
import kotlin.math.roundToInt

/**
 * While started, copies [view]'s picture into a 96 px wide bitmap every [PERIOD_MS] on its own thread, runs
 * [LetterboxDetector] on it, and hands the result and its uptime to [onSample] on the main thread.
 * [start] and [stop] are main thread only.
 */
class ContentSampler(private val view: SurfaceView, private val onSample: (ContentRect?, Long) -> Unit) {
    private val main = Handler(Looper.getMainLooper())
    private val thread = HandlerThread("AirScreenSampler").apply { start() }
    private val worker = Handler(thread.looper)

    /** Identifies the current run; a sample from an earlier run is dropped. */
    @Volatile private var run: Any? = null

    fun start(stream: Size) {
        stop()
        val token = Any().also { run = it }
        val width = WIDTH
        val height = maxOf(1, (WIDTH * stream.height / stream.width.toDouble()).roundToInt())
        val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
        val pixels = IntArray(width * height)
        val copyMs = ArrayList<Double>()
        worker.post(object : Runnable {
            override fun run() {
                if (run !== token) return
                val begin = SystemClock.elapsedRealtimeNanos()
                try {
                    PixelCopy.request(view, bitmap, { result ->
                        copyMs += (SystemClock.elapsedRealtimeNanos() - begin) / 1e6
                        if (copyMs.size == STATS_EVERY) {
                            copyMs.sort()
                            Log.i(TAG, "pixelcopy ${width}x$height ms: median %.2f max %.2f"
                                .format(copyMs[copyMs.size / 2], copyMs.last()))
                            copyMs.clear()
                        }
                        if (result == PixelCopy.SUCCESS) detect(token, bitmap, pixels)
                        if (run === token) worker.postDelayed(this, PERIOD_MS)
                    }, worker)
                } catch (e: IllegalArgumentException) {
                    // The surface is not valid yet, or no longer.
                    if (run === token) worker.postDelayed(this, PERIOD_MS)
                }
            }
        })
    }

    fun stop() {
        run = null
        worker.removeCallbacksAndMessages(null)
    }

    fun quit() {
        stop()
        thread.quitSafely()
    }

    private fun detect(token: Any, bitmap: Bitmap, pixels: IntArray) {
        bitmap.getPixels(pixels, 0, bitmap.width, 0, 0, bitmap.width, bitmap.height)
        for (i in pixels.indices) pixels[i] = LetterboxDetector.luma(pixels[i])
        val rect = LetterboxDetector.detect(pixels, bitmap.width, bitmap.height)
        val now = SystemClock.uptimeMillis()
        main.post { if (run === token) onSample(rect, now) }
    }

    private companion object {
        const val TAG = "AirScreenZoom"
        const val WIDTH = 96
        const val PERIOD_MS = 500L
        const val STATS_EVERY = 20
    }
}
