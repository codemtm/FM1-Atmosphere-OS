# FM1 Atmosphere OS — Final Project Specification

## Sound direction
- 50% ambient / 50% deep dub techno.
- Ambient projects: 80–90 BPM, evolving pads, drones, arpeggios, sub-bass and textures.
- Dub projects: 88–100 BPM, one or two principal deep chord voicings per project, transformed by filter, saturation, dub delay and space reverb.
- Original sound design; references are treated only as broad genre/aesthetic direction.

## Performance architecture
Target project mixer: DRUM / BASS / CHORD / PAD / ARP / TEXTURE / LEAD-FM / FX.
Global performance macros: SPACE / DUB / MOTION / COLOR.
Modes implemented in the current source prototype include ARP and CHORD, MIDI/key input, persistent settings, black/olive/yellow GUI, SPACE diffusion/reverb, DUB feedback delay, MOTION timing and COLOR tone/saturation.

## 16 project set
01 Baltic Fog — 82 BPM — ambient
02 Zulawy Atmosphere — 84 BPM — ambient
03 Coastal Drone — 80 BPM — ambient
04 Endless Baltic — 86 BPM — ambient
05 Northern Lights — 88 BPM — ambient
06 Cold Morning — 80 BPM — ambient
07 Vistula Delta — 85 BPM — ambient
08 Amber Sky — 90 BPM — ambient
09 Deep Harbour — 92 BPM — deep dub
10 Black Water — 94 BPM — deep dub
11 Baltic Chord — 96 BPM — deep dub
12 Night Channel — 98 BPM — deep dub
13 Port of Elblag — 100 BPM — deep dub
14 Fog Signal — 90 BPM — atmospheric dub
15 Tape Memories — 88 BPM — ambient dub
16 Petla Zulawska — 92 BPM — deep ambient dub

## Build status
This package is the final SOURCE/DESIGN package assembled from the working Atmosphere v0.4 prototype plus the 16 approved-direction audio mockups. It is NOT a flash-ready FM-1 firmware image.

The current source still uses a polyphonic sine test voice in place of the intended production 6-operator FM engine. A target build also requires the JieLi pi32v2 toolchain / AC79 SDK; that toolchain is not installed in the current build environment. Therefore no .fwsc in this package should be represented as tested Atmosphere OS firmware for hardware installation.

Before hardware flashing, remaining engineering gates are: production 6-op FM engine/preset bank, true multitrack project/sequencer implementation, target pi32v2 build, emulator/OTA validation, and hardware smoke test/recovery verification.
