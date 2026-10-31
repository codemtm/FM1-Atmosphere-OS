# FM1 Atmosphere OS v0.4

## Added in v0.4
- COLOR is now real DSP: integer one-pole tone filter plus soft tape-style saturation.
- ARP button toggles a working held-key arpeggiator; MOTION controls step time (~70–310 ms).
- SEQ button toggles CHORD mode; each trigger adds fifth + octave where range permits.
- ARP and CHORD states are stored with the Atmosphere macro settings.
- GUI shows ARP/CHORD state.
- Host smoke-test script: `tests/scripts/atmosphere_v04.txt`.

## Verification
Built and run with Dinghy host harness. Generated stereo WAV and PNG screen captures successfully.
This is still source/host-test firmware, not a pi32v2 `.fwsc` image and must not be flashed to FM-1.
