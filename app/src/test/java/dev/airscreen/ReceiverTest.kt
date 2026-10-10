package dev.airscreen

import org.junit.Assert.assertEquals
import org.junit.Test

class ReceiverTest {
    private val idle = ReceiverState.Advertising("客厅", "192.168.1.20")

    private fun run(vararg events: NativeEvent): ReceiverState =
        events.fold(ReceiverState.Starting as ReceiverState) { s, e -> reduce(s, e, idle) }

    @Test
    fun mirroringSessionReturnsToAdvertising() {
        assertEquals(idle, run(NativeEvent.Ready))
        assertEquals(
            ReceiverState.Mirroring("iPhone", VideoCodec.H265, 1170, 2532),
            run(
                NativeEvent.Ready,
                NativeEvent.AudioOnlyStarted("iPhone"),
                NativeEvent.MirrorStarted("iPhone", VideoCodec.H265),
                NativeEvent.AudioOnlyStarted("iPhone"),
                NativeEvent.VideoSize(1170, 2532),
            ),
        )
        assertEquals(
            idle,
            run(NativeEvent.Ready, NativeEvent.MirrorStarted("iPhone", VideoCodec.H264), NativeEvent.MirrorStopped),
        )
    }

    @Test
    fun audioOnlyEndsOnDisconnect() {
        assertEquals(ReceiverState.AudioOnly("iPad"), run(NativeEvent.Ready, NativeEvent.AudioOnlyStarted("iPad")))
        assertEquals(idle, run(NativeEvent.Ready, NativeEvent.AudioOnlyStarted("iPad"), NativeEvent.Disconnected))
        assertEquals(
            ReceiverState.AudioOnly("iPad"),
            run(NativeEvent.Ready, NativeEvent.AudioOnlyStarted("iPad"), NativeEvent.MirrorStopped),
        )
    }

    @Test
    fun nativeOrdinalsDecode() {
        assertEquals(
            NativeEvent.MirrorStarted("iPhone", VideoCodec.H265),
            NativeEvent.fromNative(1, "iPhone", 2, 0),
        )
        assertEquals(NativeEvent.VideoSize(1920, 1080), NativeEvent.fromNative(2, null, 1920, 1080))
        assertEquals(NativeEvent.Disconnected, NativeEvent.fromNative(5, null, 0, 0))
    }

    @Test
    fun parsesWireFormatTxt() {
        val wire = byteArrayOf(4) + "ch=2".toByteArray() + byteArrayOf(7) + "pw=fals".toByteArray() +
            byteArrayOf(3) + "pk=".toByteArray()
        assertEquals(
            listOf("ch" to "2", "pw" to "fals", "pk" to ""),
            ServiceAdvertiser.parseTxt(wire),
        )
    }
}
