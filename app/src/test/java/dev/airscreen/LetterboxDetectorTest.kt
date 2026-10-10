package dev.airscreen

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class LetterboxDetectorTest {
    private val iPadVideo = ContentRect(0f, 0.125f, 1f, 0.875f)

    /** A [width]x[height] luma grid: picture texture everywhere, then [paint] draws on it. */
    private fun grid(width: Int, height: Int, paint: (IntArray) -> Unit = {}): IntArray {
        val luma = IntArray(width * height) { i -> 60 + (i * 37 + i / width * 11) % 160 }
        paint(luma)
        return luma
    }

    /** The iPad's 4:3 grid with a 16:9 picture between 9-row bars, then [paint] draws on it. */
    private fun letterboxed(paint: (IntArray) -> Unit = {}) = grid(96, 72) {
        it.fillRows(0..8, 3)
        it.fillRows(63..71, 12)
        paint(it)
    }

    private fun IntArray.fillRows(rows: IntRange, value: Int, columns: IntRange = 0..95) =
        rows.forEach { y -> columns.forEach { x -> this[y * 96 + x] = value } }

    private fun IntArray.fillColumns(width: Int, columns: IntRange, value: Int) =
        indices.filter { it % width in columns }.forEach { this[it] = value }

    private fun detect(luma: IntArray, width: Int = 96, height: Int = 72, held: ContentRect? = null) =
        LetterboxDetector.detect(luma, width, height, held)?.let { r ->
            listOf(r.left, r.top, r.right, r.bottom).map { Math.round(it * 1000) / 1000.0 }
        }

    @Test
    fun iPadLetterbox() {
        assertEquals(listOf(0.0, 0.125, 1.0, 0.875), detect(letterboxed()))
    }

    @Test
    fun iPhonePillarbox() {
        val luma = grid(96, 44) { it.fillColumns(96, 0..7, 0); it.fillColumns(96, 88..95, 0) }
        assertEquals(listOf(0.083, 0.0, 0.917, 1.0), detect(luma, 96, 44))
    }

    @Test
    fun sparseOverlaysInTheBarsKeepThem() {
        val luma = letterboxed {
            it.fillRows(2..3, 200, 4..30)
            it.fillRows(66..66, 180, 4..91)
            it.fillRows(0..20, 50, 60..90)
        }
        assertEquals(listOf(0.0, 0.125, 1.0, 0.875), detect(luma))
    }

    @Test
    fun anOverlayAcrossOneBarKeepsTheHeldBarsButStartsNoNewOnes() {
        val luma = letterboxed { it.fillRows(0..8, 120) }
        assertEquals(listOf(0.0, 0.0, 1.0, 1.0), detect(luma))
        assertEquals(listOf(0.0, 0.125, 1.0, 0.875), detect(luma, held = iPadVideo))
    }

    @Test
    fun aDarkEdgeOfThePictureKeepsTheHeldBarsButStartsNoNewOnes() {
        val luma = letterboxed { it.fillRows(9..14, 0) }
        assertEquals(listOf(0.0, 0.0, 1.0, 1.0), detect(luma))
        assertEquals(listOf(0.0, 0.125, 1.0, 0.875), detect(luma, held = iPadVideo))
    }

    @Test
    fun brightUiDropsTheHeldBars() {
        assertEquals(listOf(0.0, 0.0, 1.0, 1.0), detect(grid(96, 72), held = iPadVideo))
    }

    @Test
    fun darkUiHeaderAndTabBarAreNotBars() {
        val luma = grid(96, 72) { it.fillRows(0..5, 0); it.fillRows(67..71, 0) }
        assertEquals(listOf(0.0, 0.0, 1.0, 1.0), detect(luma))
    }

    @Test
    fun aDarkBandAlongOneEdgeIsNotABar() {
        val luma = grid(96, 72) { it.fillRows(0..15, 0) }
        assertEquals(listOf(0.0, 0.0, 1.0, 1.0), detect(luma))
    }

    @Test
    fun barsThinnerThanTwoPercentAreIgnored() {
        val luma = grid(96, 72) { it.fillRows(0..0, 0); it.fillRows(71..71, 0) }
        assertEquals(listOf(0.0, 0.0, 1.0, 1.0), detect(luma))
    }

    @Test
    fun darkSceneIsUnknown() {
        val luma = IntArray(96 * 72) { 10 }.also { it.fillRows(30..40, 150) }
        assertNull(detect(luma))
        assertNull(detect(luma, held = iPadVideo))
    }

    @Test
    fun lumaUsesBt601Weights() {
        assertEquals(0, LetterboxDetector.luma(0xff000000.toInt()))
        assertEquals(255, LetterboxDetector.luma(0xffffffff.toInt()))
        assertEquals(76, LetterboxDetector.luma(0xffff0000.toInt()))
        assertEquals(149, LetterboxDetector.luma(0xff00ff00.toInt()))
    }
}
