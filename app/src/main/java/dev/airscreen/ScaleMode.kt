package dev.airscreen

import kotlin.math.roundToInt

/** How a stream wider than the screen maps onto it. Every other stream (portrait, iPad 4:3) uses [Fit]. */
enum class ScaleMode { Fit, Fill }

data class Size(val width: Int, val height: Int)

/**
 * Size of the stream's view inside a [parentWidth]x[parentHeight] parent, aspect preserved.
 * Fit lies inside the parent; Fill covers it and overflows one axis, to be centered and clipped.
 */
fun streamViewSize(parentWidth: Int, parentHeight: Int, streamWidth: Int, streamHeight: Int, wide: ScaleMode): Size {
    if (streamWidth <= 0 || streamHeight <= 0) return Size(parentWidth, parentHeight)
    val mode = if (fillApplies(parentWidth, parentHeight, streamWidth, streamHeight)) wide else ScaleMode.Fit
    val parentIsWider = parentWidth.toLong() * streamHeight > parentHeight.toLong() * streamWidth
    return if (parentIsWider == (mode == ScaleMode.Fit)) {
        Size((parentHeight.toLong() * streamWidth / streamHeight.toDouble()).roundToInt(), parentHeight)
    } else {
        Size(parentWidth, (parentWidth.toLong() * streamHeight / streamWidth.toDouble()).roundToInt())
    }
}

/**
 * Fill only crops the sides of a stream wider than the screen (iPhone landscape, ~2.17:1). Filling a
 * narrower one would crop its top and bottom, where an iPad keeps its status bar and tab bars.
 */
fun fillApplies(parentWidth: Int, parentHeight: Int, streamWidth: Int, streamHeight: Int): Boolean =
    streamWidth > 0 && streamHeight > 0 && streamWidth.toLong() * parentHeight > parentWidth.toLong() * streamHeight
