# Vendored / first-party DSP sources

## First-party (this project's own code) — all of `src/`
`fx_engine.c` (the `mpc_engine` effect adapter) and the four DSP cores —
`reverb_c.c`, `delay_tape_c.c`, `sat_c.c`, `chorus_c.c` — are written for this project and are
covered by this repo's MIT LICENSE. They are **not** third-party and are maintained here directly.
(The same cores also ship in the author's *Mutable Vibe* instrument; they are first-party there too.)

There is **no vendored third-party code** in this repository — unlike instrument ports that vendor an
upstream DSP tree, Vibe FX ships only its own sources.

## Acknowledgements (prior art, no code included)
The cores are original implementations, but they stand on well-known prior art and we credit it:

- **Reverb** (`reverb_c.c`) — an 8-tap feedback-delay-network hall (Hadamard feedback, modulated
  delay lines, early reflections). It grew out of a plate reverb after Jon Dattorro, *"Effect Design,
  Part 1: Reverberator and Other Filters"* (JAES, 1997); the modulated-FDN rework aimed for the
  character of hall reverbs like Dragonfly. No Freeverb/Freeverb3/zita or Dragonfly code is used.
- **Delay** (`delay_tape_c.c`) — a tape-style stereo delay (flutter, head-bump EQ, tape saturation
  in the feedback path) inspired by the delays of **Airwindows** (Chris Johnson). Independent code.
- **Chorus** (`chorus_c.c`) — a stereo BBD chorus modelled on the **Roland Juno** series.
- **Saturation** (`sat_c.c`) — a `tanh` tape-style waveshaper with gentle even-harmonic asymmetry.
