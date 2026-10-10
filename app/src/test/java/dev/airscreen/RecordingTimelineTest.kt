package dev.airscreen

import java.io.DataInputStream
import java.io.File
import org.junit.Assume.assumeTrue
import org.junit.Test

/**
 * Replays the zoom decisions over a real recording's frames, sampled every 500 ms as the app does. Runs only
 * when AIRSCREEN_FRAMES names a directory from `scripts/zoom-timeline.py`, and writes timeline.txt there.
 */
class RecordingTimelineTest {
    @Test
    fun timeline() {
        val dir = System.getenv("AIRSCREEN_FRAMES")?.let(::File)
        assumeTrue(dir != null)
        val arrivals = File(dir, "times.txt").readLines().map { (it.toDouble() * 1000).toLong() }
        val out = StringBuilder()
        var state: ZoomState = ZoomState.Idle
        var stream: Size? = null
        var frame = 0
        for (now in 0..arrivals.last() step 500) {
            while (frame + 1 < arrivals.size && arrivals[frame + 1] <= now) frame++
            val (size, luma) = readPpm(File(dir, "%05d.ppm".format(frame + 1)))
            if (size != stream) {
                stream = size
                state = ZoomState.Idle
            }
            val sample = LetterboxDetector.detect(luma, size.width, size.height)
            val next = state.next(sample, now)
            out.append("%6.1f s  frame %5d  %-40s  %s%s\n".format(
                now / 1000.0, frame + 1, sample?.let(::format) ?: "dark", next::class.simpleName,
                if (next.zoom != state.zoom) "   <== ${if (next.zoom != null) "ZOOM IN" else "ZOOM OUT"}" else "",
            ))
            state = next
        }
        File(dir, "timeline.txt").writeText(out.toString())
    }

    private fun format(r: ContentRect) =
        if (r == ContentRect.Whole) "whole" else "%.3f %.3f %.3f %.3f".format(r.left, r.top, r.right, r.bottom)

    private fun readPpm(file: File): Pair<Size, IntArray> = DataInputStream(file.inputStream().buffered()).use { s ->
        val header = generateSequence { token(s) }.take(4).toList()
        check(header[0] == "P6" && header[3] == "255") { "not an 8-bit PPM: $file" }
        val size = Size(header[1].toInt(), header[2].toInt())
        val rgb = ByteArray(size.width * size.height * 3).also { s.readFully(it) }
        size to IntArray(size.width * size.height) { i ->
            val argb = (rgb[3 * i].toInt() and 0xff shl 16) or (rgb[3 * i + 1].toInt() and 0xff shl 8) or
                (rgb[3 * i + 2].toInt() and 0xff)
            LetterboxDetector.luma(argb)
        }
    }

    private fun token(s: DataInputStream): String {
        val sb = StringBuilder()
        while (true) {
            val c = s.read().toChar()
            if (c.isWhitespace()) { if (sb.isNotEmpty()) return sb.toString() } else sb.append(c)
        }
    }
}
