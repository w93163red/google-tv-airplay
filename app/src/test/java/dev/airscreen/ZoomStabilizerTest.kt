package dev.airscreen

import org.junit.Assert.assertEquals
import org.junit.Test

class ZoomStabilizerTest {
    private val letterbox = ContentRect(0f, 0.125f, 1f, 0.875f)
    private val letterboxJitter = ContentRect(0f, 0.111f, 1f, 0.889f)
    private val wideMovie = ContentRect(0f, 0.222f, 1f, 0.778f)
    private val whole = ContentRect.Whole

    /** The zoom decided after each sample, with samples 500 ms apart as the app takes them. */
    private fun zooms(vararg samples: ContentRect?): List<ContentRect?> {
        var state: ZoomState = ZoomState.Idle
        return samples.mapIndexed { i, sample -> state.next(sample, i * 500L).also { state = it }.zoom }
    }

    @Test
    fun zoomsInOnceTheSameBarsHoldForOneAndAHalfSeconds() {
        assertEquals(
            listOf(null, null, null, letterbox, letterbox),
            zooms(letterbox, letterboxJitter, letterbox, letterbox, letterboxJitter),
        )
    }

    @Test
    fun noBarsNeverZoom() {
        assertEquals(listOf(null, null, null, null, null), zooms(whole, whole, whole, whole, whole))
    }

    @Test
    fun aFrameWithoutBarsRestartsTheWait() {
        assertEquals(
            listOf(null, null, null, null, null, null, letterbox),
            zooms(letterbox, letterbox, whole, letterbox, letterbox, letterbox, letterbox),
        )
    }

    @Test
    fun differentBarsRestartTheWait() {
        assertEquals(
            listOf(null, null, null, null, null, wideMovie),
            zooms(letterbox, letterbox, wideMovie, wideMovie, wideMovie, wideMovie),
        )
    }

    @Test
    fun aDarkFrameNeitherStartsNorResetsAnything() {
        assertEquals(listOf(null, null, null, letterbox), zooms(letterbox, null, null, letterbox))
        assertEquals(listOf(null, null, null), zooms(null, null, null))
    }

    @Test
    fun zoomsOutOnceTheBarsShowPictureForOneSecond() {
        assertEquals(
            listOf(null, null, null, letterbox, letterbox, letterbox, null),
            zooms(letterbox, letterbox, letterbox, letterbox, whole, whole, whole),
        )
    }

    @Test
    fun controlsFlashingOverABarDoNotZoomOut() {
        assertEquals(
            listOf(null, null, null, letterbox, letterbox, letterbox, letterbox, letterbox, letterbox),
            zooms(letterbox, letterbox, letterbox, letterbox, whole, letterbox, whole, whole, letterbox),
        )
    }

    @Test
    fun aDarkerPictureInsideTheBarsStaysZoomed() {
        assertEquals(
            listOf(null, null, null, letterbox, letterbox, letterbox, letterbox),
            zooms(letterbox, letterbox, letterbox, letterbox, wideMovie, wideMovie, null),
        )
    }

    @Test
    fun leavingIntoOtherBarsZoomsToThemAfterTheWait() {
        assertEquals(
            listOf(null, null, null, wideMovie, wideMovie, wideMovie, null, null, null, letterbox),
            zooms(wideMovie, wideMovie, wideMovie, wideMovie, letterbox, letterbox, letterbox, letterbox, letterbox,
                letterbox),
        )
    }
}
