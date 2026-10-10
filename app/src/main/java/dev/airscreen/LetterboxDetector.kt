package dev.airscreen

import kotlin.math.min

/**
 * Finds the active picture inside letterbox or pillarbox bars in a small luma grid (0..255, row-major) of one
 * stream frame. A bar is a run of rows or columns at an edge that are black but for a stray pixel or two.
 */
object LetterboxDetector {
    /** Luma at or below this is bar black. */
    const val BLACK = 24

    /**
     * The picture inside the bars, [ContentRect.Whole] when there are none, or null when the center is dark too,
     * because then a dark scene is indistinguishable from bars.
     */
    fun detect(luma: IntArray, width: Int, height: Int): ContentRect? {
        if (!centerHasContent(luma, width, height)) return null
        val rowDark = { y: Int -> brightCount(luma, y * width, 1, width) <= width / STRAY_DIVISOR }
        val columnDark = { x: Int -> brightCount(luma, x, width, height) <= height / STRAY_DIVISOR }
        val vertical = barThickness(height, rowDark)
        val horizontal = barThickness(width, columnDark)
        return ContentRect(
            horizontal.toFloat() / width, vertical.toFloat() / height,
            1 - horizontal.toFloat() / width, 1 - vertical.toFloat() / height,
        )
    }

    /** RGB to luma, BT.601 weights. */
    fun luma(argb: Int): Int = (77 * (argb shr 16 and 0xff) + 150 * (argb shr 8 and 0xff) + 29 * (argb and 0xff)) shr 8

    /**
     * A player centers its picture, so bars come in equal pairs. Taking the thinner of the two keeps a dark edge
     * of the picture from widening a bar, and keeps a dark band along one edge of a UI from counting as one.
     */
    private fun barThickness(lines: Int, dark: (Int) -> Boolean): Int {
        val limit = lines / 3
        val leading = (0 until limit).firstOrNull { !dark(it) } ?: limit
        val trailing = (0 until limit).firstOrNull { !dark(lines - 1 - it) } ?: limit
        val bar = min(leading, trailing)
        return if (bar * 50 < lines) 0 else bar
    }

    private fun centerHasContent(luma: IntArray, width: Int, height: Int): Boolean {
        var bright = 0
        var total = 0
        for (y in height / 4 until height - height / 4) {
            bright += brightCount(luma, y * width + width / 4, 1, width - 2 * (width / 4))
            total += width - 2 * (width / 4)
        }
        return bright * 2 >= total
    }

    private fun brightCount(luma: IntArray, start: Int, step: Int, count: Int): Int {
        var bright = 0
        for (i in 0 until count) if (luma[start + i * step] > BLACK) bright++
        return bright
    }

    /** A line may hold 1 bright pixel per this many and still count as black. */
    private const val STRAY_DIVISOR = 48
}
