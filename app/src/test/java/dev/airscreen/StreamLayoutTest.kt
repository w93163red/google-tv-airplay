package dev.airscreen

import org.junit.Assert.assertEquals
import org.junit.Test

class StreamLayoutTest {
    private val tv = Size(1920, 1080)
    private val iPad = Size(1442, 1080)
    private val iPhoneLandscape = Size(1920, 886)

    @Test
    fun iPadLetterboxedVideoFillsTheScreen() {
        val video16x9 = ContentRect(0f, 0.12448f, 1f, 0.87552f)
        assertEquals(Placement(1920, 1438, 0, -179), streamPlacement(tv, iPad, ZoomMode.Smart, video16x9))
    }

    @Test
    fun iPadWithoutBarsShowsTheWholeScreen() {
        assertEquals(Placement(1442, 1080, 239, 0), streamPlacement(tv, iPad, ZoomMode.Smart, null))
    }

    @Test
    fun iPhonePillarboxedVideoFillsTheScreen() {
        val video16x9 = ContentRect(0.0898f, 0f, 0.9102f, 1f)
        assertEquals(Placement(2340, 1080, -210, 0), streamPlacement(tv, iPhoneLandscape, ZoomMode.Smart, video16x9))
    }

    @Test
    fun iPhoneBarsNarrowedByPlayerControlsStillFill() {
        val narrowedBars = ContentRect(0.0625f, 0f, 0.9375f, 1f)
        assertEquals(Placement(2340, 1080, -210, 0), streamPlacement(tv, iPhoneLandscape, ZoomMode.Smart, narrowedBars))
    }

    @Test
    fun iPhoneLandscapeUiWithoutBarsCropsTheSides() {
        assertEquals(Placement(2340, 1080, -210, 0), streamPlacement(tv, iPhoneLandscape, ZoomMode.Smart, null))
    }

    @Test
    fun fullModeFitsTheWholeStreamWhateverWasDetected() {
        val video16x9 = ContentRect(0f, 0.12448f, 1f, 0.87552f)
        assertEquals(Placement(1442, 1080, 239, 0), streamPlacement(tv, iPad, ZoomMode.Full, video16x9))
        assertEquals(Placement(1920, 886, 0, 97), streamPlacement(tv, iPhoneLandscape, ZoomMode.Full, null))
    }

    @Test
    fun portraitStreamFits() {
        assertEquals(Placement(498, 1080, 711, 0), streamPlacement(tv, Size(886, 1920), ZoomMode.Smart, null))
    }

    @Test
    fun unknownStreamSizeFillsParent() {
        assertEquals(Placement(1920, 1080, 0, 0), streamPlacement(tv, Size(0, 0), ZoomMode.Smart, null))
    }
}
