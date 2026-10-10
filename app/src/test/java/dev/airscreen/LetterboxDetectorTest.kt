package dev.airscreen

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class LetterboxDetectorTest {
    /** A [width]x[height] luma grid: picture texture everywhere, then [paint] draws on it. */
    private fun grid(width: Int, height: Int, paint: (IntArray) -> Unit = {}): IntArray {
        val luma = IntArray(width * height) { i -> 60 + (i * 37 + i / width * 11) % 160 }
        paint(luma)
        return luma
    }

    private fun IntArray.fillRows(width: Int, rows: IntRange, value: Int) =
        rows.forEach { y -> fill(value, y * width, (y + 1) * width) }

    private fun IntArray.fillColumns(width: Int, columns: IntRange, value: Int) =
        indices.filter { it % width in columns }.forEach { this[it] = value }

    private fun detect(luma: IntArray, width: Int, height: Int) =
        LetterboxDetector.detect(luma, width, height)?.let { r ->
            listOf(r.left, r.top, r.right, r.bottom).map { Math.round(it * 1000) / 1000.0 }
        }

    @Test
    fun iPadLetterbox() {
        val luma = grid(96, 72) { it.fillRows(96, 0..8, 3); it.fillRows(96, 63..71, 12) }
        assertEquals(listOf(0.0, 0.125, 1.0, 0.875), detect(luma, 96, 72))
    }

    @Test
    fun iPhonePillarbox() {
        val luma = grid(96, 44) { it.fillColumns(96, 0..7, 0); it.fillColumns(96, 88..95, 0) }
        assertEquals(listOf(0.083, 0.0, 0.917, 1.0), detect(luma, 96, 44))
    }

    @Test
    fun aStrayBrightPixelInABarIsNoise() {
        val luma = grid(96, 72) {
            it.fillRows(96, 0..8, 0)
            it.fillRows(96, 63..71, 0)
            it[4 * 96 + 50] = 200
        }
        assertEquals(listOf(0.0, 0.125, 1.0, 0.875), detect(luma, 96, 72))
    }

    @Test
    fun uiWithContentToTheEdgesHasNoBars() {
        assertEquals(listOf(0.0, 0.0, 1.0, 1.0), detect(grid(96, 72), 96, 72))
    }

    @Test
    fun aDarkBandAlongOneEdgeIsNotABar() {
        val luma = grid(96, 72) { it.fillRows(96, 0..15, 0) }
        assertEquals(listOf(0.0, 0.0, 1.0, 1.0), detect(luma, 96, 72))
    }

    @Test
    fun aDarkEdgeOfThePictureDoesNotWidenItsBar() {
        val luma = grid(96, 72) { it.fillRows(96, 0..14, 0); it.fillRows(96, 63..71, 0) }
        assertEquals(listOf(0.0, 0.125, 1.0, 0.875), detect(luma, 96, 72))
    }

    @Test
    fun barsThinnerThanTwoPercentAreIgnored() {
        val luma = grid(96, 72) { it.fillRows(96, 0..0, 0); it.fillRows(96, 71..71, 0) }
        assertEquals(listOf(0.0, 0.0, 1.0, 1.0), detect(luma, 96, 72))
    }

    @Test
    fun playerControlsInABarCountAsPicture() {
        val luma = grid(96, 72) {
            it.fillRows(96, 0..8, 0)
            it.fillRows(96, 63..71, 0)
            it.fill(180, 66 * 96 + 4, 66 * 96 + 92)
        }
        assertEquals(listOf(0.0, 0.069, 1.0, 0.931), detect(luma, 96, 72))
    }

    @Test
    fun darkSceneIsUnknown() {
        val luma = IntArray(96 * 72) { 10 }.also { it.fillRows(96, 30..40, 150) }
        assertNull(detect(luma, 96, 72))
    }

    @Test
    fun lumaUsesBt601Weights() {
        assertEquals(0, LetterboxDetector.luma(0xff000000.toInt()))
        assertEquals(255, LetterboxDetector.luma(0xffffffff.toInt()))
        assertEquals(76, LetterboxDetector.luma(0xffff0000.toInt()))
        assertEquals(149, LetterboxDetector.luma(0xff00ff00.toInt()))
    }
}
