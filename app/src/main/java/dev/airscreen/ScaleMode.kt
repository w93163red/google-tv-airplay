package dev.airscreen

import kotlin.math.roundToInt

/** How a landscape stream maps onto the screen. Portrait streams always use [Fit]. */
enum class ScaleMode { Fit, Fill }

data class Size(val width: Int, val height: Int)

/**
 * Size of the stream's view inside a [parentWidth]x[parentHeight] parent, aspect preserved.
 * Fit lies inside the parent; Fill covers it and overflows one axis, to be centered and clipped.
 */
fun streamViewSize(parentWidth: Int, parentHeight: Int, streamWidth: Int, streamHeight: Int, landscape: ScaleMode): Size {
    if (streamWidth <= 0 || streamHeight <= 0) return Size(parentWidth, parentHeight)
    val mode = if (streamHeight > streamWidth) ScaleMode.Fit else landscape
    val parentIsWider = parentWidth.toLong() * streamHeight > parentHeight.toLong() * streamWidth
    return if (parentIsWider == (mode == ScaleMode.Fit)) {
        Size((parentHeight.toLong() * streamWidth / streamHeight.toDouble()).roundToInt(), parentHeight)
    } else {
        Size(parentWidth, (parentWidth.toLong() * streamHeight / streamWidth.toDouble()).roundToInt())
    }
}
