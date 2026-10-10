package dev.airscreen

import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * Finds the active picture inside letterbox or pillarbox bars in a small luma grid (0..255, row-major) of one
 * stream frame.
 *
 * A bar is an edge band that is mostly black and ends at a sharp, full-width boundary into picture. Player
 * controls, titles and menus drawn over a bar leave it mostly black, so they do not break it. How black a band
 * is cannot tell bars from a dark-mode UI (YouTube's watch page has edge bands 70-90% black, the same as bars
 * under its controls), so a new pair of bars needs that boundary at the same depth on both edges, as a centered
 * player draws them.
 */
object LetterboxDetector {
    /** Luma at or below this is bar black. */
    const val BLACK = 24

    /**
     * The picture inside the bars, [ContentRect.Whole] when there are none, or null when the center is dark too,
     * because then a dark scene is indistinguishable from bars. [held], the rect currently zoomed to, is kept
     * while one edge of each of its bar pairs still shows its boundary, so a dark edge of the picture or an
     * overlay across one bar does not drop it.
     */
    fun detect(luma: IntArray, width: Int, height: Int, held: ContentRect? = null): ContentRect? {
        if (!centerHasContent(luma, width, height)) return null
        val rows = Edges(DoubleArray(height) { y -> blackFraction(luma, y * width, 1, width) })
        val columns = Edges(DoubleArray(width) { x -> blackFraction(luma, x, width, height) })
        if (held != null && rows.holds(held.top) && columns.holds(held.left)) return held
        val vertical = rows.pair()
        val horizontal = columns.pair()
        val aspect = (width - 2.0 * horizontal) / (height - 2.0 * vertical)
        if (VIDEO_ASPECTS.none { abs(aspect / it - 1) <= ASPECT_TOLERANCE }) return ContentRect.Whole
        return ContentRect(
            horizontal.toFloat() / width, vertical.toFloat() / height,
            1 - horizontal.toFloat() / width, 1 - vertical.toFloat() / height,
        )
    }

    /** RGB to luma, BT.601 weights. */
    fun luma(argb: Int): Int = (77 * (argb shr 16 and 0xff) + 150 * (argb shr 8 and 0xff) + 29 * (argb and 0xff)) shr 8

    /** The black fraction of each line across one axis, read from either end. */
    private class Edges(private val lines: DoubleArray) {
        private val count = lines.size
        private val reversed = lines.reversedArray()

        /** The thinnest bar both ends show, within a line of each other, or 0. */
        fun pair(): Int {
            for (t in 1..count / 3) {
                if (t * 50 < count || !boundary(lines, t)) continue
                val trailing = listOf(t, t - 1, t + 1).firstOrNull { boundary(reversed, it) } ?: continue
                if (minOf(t, trailing) * 50 >= count) return minOf(t, trailing)
            }
            return 0
        }

        /** Whether a bar [fraction] of the axis deep still shows its boundary at either end. */
        fun holds(fraction: Float): Boolean {
            val t = (fraction * count).roundToInt()
            return t == 0 || boundary(lines, t) || boundary(reversed, t)
        }

        /** Lines 0 until [t] are a mostly black band whose last line is black and whose next two are picture. */
        private fun boundary(lines: DoubleArray, t: Int): Boolean {
            if (t < 1 || t + 1 >= count) return false
            return (0 until t).sumOf { lines[it] } / t >= BAND_BLACK && lines[t - 1] >= EDGE_BLACK &&
                lines[t] <= PICTURE_BLACK && lines[t + 1] <= PICTURE_BLACK
        }
    }

    private fun centerHasContent(luma: IntArray, width: Int, height: Int): Boolean {
        var black = 0.0
        val rows = height / 4 until height - height / 4
        for (y in rows) black += blackFraction(luma, y * width + width / 4, 1, width - 2 * (width / 4))
        return black / (rows.last - rows.first + 1) <= 0.5
    }

    private fun blackFraction(luma: IntArray, start: Int, step: Int, count: Int): Double {
        var black = 0
        for (i in 0 until count) if (luma[start + i * step] <= BLACK) black++
        return black.toDouble() / count
    }

    /**
     * A bar band's least black fraction. YouTube's quality menu over the iPad's top bar, with the status bar and
     * title row, leaves it 65% black; clean bars are 100%.
     */
    private const val BAND_BLACK = 0.6

    /** The last bar line's least black fraction; under the same menu it is 69%. */
    private const val EDGE_BLACK = 0.6

    /** The most black fraction of each of the first two picture lines. */
    private const val PICTURE_BLACK = 0.5

    /**
     * Picture aspects a player letterboxes or pillarboxes. A dark-mode UI's header and tab bar frame an
     * arbitrary one: YouTube's home feed on an iPad frames 1.55.
     */
    private val VIDEO_ASPECTS = listOf(4.0 / 3, 16.0 / 9, 1.85, 2.0, 2.39, 9.0 / 16)

    /** A grid line is 1-2% of the picture, so its bars land this close to the true aspect. */
    private const val ASPECT_TOLERANCE = 0.04
}
