package dev.airscreen

import kotlin.math.min
import kotlin.math.roundToInt

/** How the mirrored stream maps onto the screen. The remote's OK button toggles it; the choice persists. */
enum class ZoomMode {
    /**
     * Fits the detected picture inside letterbox or pillarbox bars to the screen. Without bars, a stream wider
     * than the screen (iPhone landscape, ~2.17:1) fills it by cropping the sides, and any other stream fits,
     * because cropping an iPad's top and bottom would cut off its status bar and tab bars.
     */
    Smart,

    /** The whole stream, never cropped. */
    Full,
}

/** The active picture inside a stream, in normalized stream coordinates: 0..1 on each axis. */
data class ContentRect(val left: Float, val top: Float, val right: Float, val bottom: Float) {
    companion object {
        val Whole = ContentRect(0f, 0f, 1f, 1f)
    }
}

data class Size(val width: Int, val height: Int)

/** The stream view's size and its top-left corner relative to the parent's. The screen clips any overflow. */
data class Placement(val width: Int, val height: Int, val x: Int, val y: Int)

/** Places the stream so that the part of it [mode] and [content] select is centered and fitted in [parent]. */
fun streamPlacement(parent: Size, stream: Size, mode: ZoomMode, content: ContentRect?): Placement {
    if (stream.width <= 0 || stream.height <= 0) return Placement(parent.width, parent.height, 0, 0)
    val fitScale = { r: ContentRect ->
        min(
            parent.width / ((r.right - r.left) * stream.width.toDouble()),
            parent.height / ((r.bottom - r.top) * stream.height.toDouble()),
        )
    }
    // Bars only ever zoom in further than the no-bars rule, which already crops a wide picture's sides.
    val shown = when (mode) {
        ZoomMode.Full -> ContentRect.Whole
        ZoomMode.Smart -> listOfNotNull(content, centerSliceAtMostAsWideAs(parent, stream)).maxBy(fitScale)
    }
    val scale = fitScale(shown)
    val width = stream.width * scale
    val height = stream.height * scale
    return Placement(
        width.roundToInt(),
        height.roundToInt(),
        (parent.width / 2.0 - (shown.left + shown.right) / 2 * width).roundToInt(),
        (parent.height / 2.0 - (shown.top + shown.bottom) / 2 * height).roundToInt(),
    )
}

/** The full-height center slice of [stream] with [parent]'s aspect, or the whole stream if it is not wider. */
private fun centerSliceAtMostAsWideAs(parent: Size, stream: Size): ContentRect {
    val fraction = parent.width.toDouble() * stream.height / (parent.height.toDouble() * stream.width)
    if (fraction >= 1) return ContentRect.Whole
    val left = ((1 - fraction) / 2).toFloat()
    return ContentRect(left, 0f, 1 - left, 1f)
}
