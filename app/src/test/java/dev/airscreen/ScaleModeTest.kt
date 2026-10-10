package dev.airscreen

import org.junit.Assert.assertEquals
import org.junit.Test

class ScaleModeTest {
    @Test
    fun landscapeIPhoneStreamOn16x9() {
        assertEquals(Size(2340, 1080), streamViewSize(1920, 1080, 1920, 886, ScaleMode.Fill))
        assertEquals(Size(1920, 886), streamViewSize(1920, 1080, 1920, 886, ScaleMode.Fit))
    }

    @Test
    fun portraitStreamAlwaysFits() {
        assertEquals(Size(498, 1080), streamViewSize(1920, 1080, 886, 1920, ScaleMode.Fill))
        assertEquals(Size(498, 1080), streamViewSize(1920, 1080, 886, 1920, ScaleMode.Fit))
    }

    @Test
    fun narrowerThanScreenStreamAlwaysFits() {
        assertEquals(Size(1440, 1080), streamViewSize(1920, 1080, 1024, 768, ScaleMode.Fit))
        assertEquals(Size(1440, 1080), streamViewSize(1920, 1080, 1024, 768, ScaleMode.Fill))
        assertEquals(Size(1442, 1080), streamViewSize(1920, 1080, 1442, 1080, ScaleMode.Fill))
    }

    @Test
    fun onlyStreamsWiderThanTheScreenCanFill() {
        assertEquals(true, fillApplies(1920, 1080, 1920, 886))
        assertEquals(false, fillApplies(1920, 1080, 1442, 1080))
        assertEquals(false, fillApplies(1920, 1080, 886, 1920))
        assertEquals(false, fillApplies(1920, 1080, 1920, 1080))
    }

    @Test
    fun unknownStreamSizeFillsParent() {
        assertEquals(Size(1920, 1080), streamViewSize(1920, 1080, 0, 0, ScaleMode.Fit))
    }
}
