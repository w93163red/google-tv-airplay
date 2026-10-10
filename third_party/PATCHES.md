# Local patches to vendored sources

## alac/ALACDecoder.cpp

`Decode` lets a partial-frame header in the packet replace `numSamples` with an unchecked
32-bit value, which then overruns `mMixBufferU`/`mMixBufferV`/`mPredictor` (sized to
`frameLength`) and the caller's output buffer. AirPlay packets come from any LAN client, so
both partial-frame reads now fail with `kALAC_ParamError` when `numSamples > mConfig.frameLength`.
