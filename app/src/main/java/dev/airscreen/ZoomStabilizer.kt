package dev.airscreen

import kotlin.math.abs

/**
 * Turns [LetterboxDetector] samples into a zoom decision. Zooms in once the same bars hold for [ZOOM_IN_MS], and
 * back out once the bars show picture for [ZOOM_OUT_MS], so player controls flashing over a bar do not flap it.
 */
sealed interface ZoomState {
    data object Idle : ZoomState
    data class Candidate(val rect: ContentRect, val since: Long) : ZoomState
    data class Zoomed(val rect: ContentRect) : ZoomState
    data class ZoomedLeaving(val rect: ContentRect, val since: Long) : ZoomState

    /** The content rect to zoom to, or null to show the stream as if nothing were detected. */
    val zoom: ContentRect?
        get() = when (this) {
            is Zoomed -> rect
            is ZoomedLeaving -> rect
            Idle, is Candidate -> null
        }

    /** The next state after [sample], the detector's result at [now] ms; a null sample says nothing. */
    fun next(sample: ContentRect?, now: Long): ZoomState {
        if (sample == null) return this
        val barred = !sample.near(ContentRect.Whole)
        return when (this) {
            Idle -> if (barred) Candidate(sample, now) else Idle
            is Candidate -> when {
                !barred -> Idle
                !sample.near(rect) -> Candidate(sample, now)
                now - since >= ZOOM_IN_MS -> Zoomed(rect)
                else -> this
            }
            is Zoomed -> if (sample.within(rect)) this else ZoomedLeaving(rect, now)
            is ZoomedLeaving -> when {
                sample.within(rect) -> Zoomed(rect)
                now - since < ZOOM_OUT_MS -> this
                barred -> Candidate(sample, now)
                else -> Idle
            }
        }
    }

    companion object {
        const val ZOOM_IN_MS = 1500L
        const val ZOOM_OUT_MS = 1000L
        const val TOLERANCE = 0.025f
    }
}

private fun ContentRect.near(other: ContentRect) =
    abs(left - other.left) <= ZoomState.TOLERANCE && abs(top - other.top) <= ZoomState.TOLERANCE &&
        abs(right - other.right) <= ZoomState.TOLERANCE && abs(bottom - other.bottom) <= ZoomState.TOLERANCE

/** True when this picture stays inside [bars]' picture, that is, the bars around it are still black. */
private fun ContentRect.within(bars: ContentRect) =
    left >= bars.left - ZoomState.TOLERANCE && top >= bars.top - ZoomState.TOLERANCE &&
        right <= bars.right + ZoomState.TOLERANCE && bottom <= bars.bottom + ZoomState.TOLERANCE
