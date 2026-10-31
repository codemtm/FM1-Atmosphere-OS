# Atmosphere OS v0.3

This revision connects the four Atmosphere macros to real integer DSP in the Dinghy audio path.

- SPACE: stereo dual-line diffusion/reverb amount and feedback.
- DUB: feedback dub-delay wet level and feedback.
- MOTION: delay-time movement/range.
- COLOR: retained in the persistent macro model for the next filter/saturation stage.
- Fixed memory only; no heap, floating point, division, screen or flash access in the audio ISR.
- Host build tested with `APP_VER=0.3 tests/run_host.sh`.

This is a source/host-test milestone, not a device-flashable release. A pi32v2/JieLi target build and emulator/device validation are still required before installation on hardware.
