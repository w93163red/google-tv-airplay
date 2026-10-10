package dev.airscreen

enum class VideoCodec { H264, H265 }

sealed interface ReceiverState {
    data object Starting : ReceiverState
    data class Advertising(val name: String, val ip: String) : ReceiverState
    data class Mirroring(val client: String, val codec: VideoCodec, val width: Int, val height: Int) : ReceiverState
    data class AudioOnly(val client: String) : ReceiverState
    data class Failed(val reason: String) : ReceiverState
}

sealed interface NativeEvent {
    data object Ready : NativeEvent
    data class MirrorStarted(val client: String, val codec: VideoCodec) : NativeEvent
    data class VideoSize(val width: Int, val height: Int) : NativeEvent
    data class AudioOnlyStarted(val client: String) : NativeEvent
    data object MirrorStopped : NativeEvent
    data object Disconnected : NativeEvent
    data class Failed(val reason: String) : NativeEvent

    companion object {
        /** Decodes native_event_t from airscreen.h; ordinals must match. */
        fun fromNative(kind: Int, text: String?, a: Int, b: Int): NativeEvent = when (kind) {
            0 -> Ready
            1 -> MirrorStarted(text.orEmpty(), if (a == 2) VideoCodec.H265 else VideoCodec.H264)
            2 -> VideoSize(a, b)
            3 -> AudioOnlyStarted(text.orEmpty())
            4 -> MirrorStopped
            5 -> Disconnected
            else -> error("unknown native event $kind")
        }
    }
}

/** [idle] is the Advertising state this receiver returns to when a session ends. */
fun reduce(state: ReceiverState, event: NativeEvent, idle: ReceiverState.Advertising): ReceiverState =
    when (event) {
        NativeEvent.Ready -> idle
        is NativeEvent.Failed -> ReceiverState.Failed(event.reason)
        is NativeEvent.MirrorStarted -> ReceiverState.Mirroring(event.client, event.codec, 0, 0)
        is NativeEvent.VideoSize ->
            if (state is ReceiverState.Mirroring) state.copy(width = event.width, height = event.height) else state
        is NativeEvent.AudioOnlyStarted ->
            if (state is ReceiverState.Mirroring) state else ReceiverState.AudioOnly(event.client)
        NativeEvent.MirrorStopped -> if (state is ReceiverState.Mirroring) idle else state
        NativeEvent.Disconnected ->
            if (state is ReceiverState.Mirroring || state is ReceiverState.AudioOnly) idle else state
    }
