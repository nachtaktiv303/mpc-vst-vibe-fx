# Vibe FX

**A native Akai MPC OS insert effect — drive, chorus, reverb and tape delay in one slot.**
Version **0.9** (public preview).

Vibe FX is a VST2 multi-effect that runs **inside MPC OS** — no bridge, no background app. Drop it on
a track as an insert; it turns with the Q-Links and saves with the project, with its own native MPC
touchscreen pages.

> Built with the [mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins) porting kit.

## What's in it

A fixed, musical signal chain (not freely routable — by design, so it fits one insert slot):

```
IN → DRIVE → CHORUS → (dry) ─┬─→ REVERB × send ─┐
                             └─→ DELAY  × send ─┴─→ OUT
```

- **Drive** — tape-style saturation front-end.
- **Chorus** — Juno-style stereo BBD chorus (rate / depth / mix).
- **Reverb** — FDN hall (decay, damping, high-pass, pre-delay) on its own send.
- **Tape delay** — free or tempo-synced (dotted divisions), bipolar tone (LP ↔ feedback HP),
  feedback with a safety cap, on its own send.

Two touchscreen pages in the Mutable Instruments style — **DRIVE / CHORUS** and **REVERB / DELAY** —
with a matching Q-Link map.

## Status

**Tested only on the MPC One (1st generation / Gen1).** That is the single device it has run on so
far. Other Gen1 MPC OS devices (Live, X, Key, Force) run the same `MPC` program and are *expected* to
behave the same, but this is **unverified** — reports welcome. Gen2 devices are reported to be more
locked down and are not supported.

Insert effects receive **no MIDI** on MPC OS, and `ppqPos` resets to 0 on every transport stop, so
tempo-sync follows the host tempo but there is no "double-stop kills the tail" trick — a single stop
simply lets the tail ring out as the input stops.

## Build & install

This port builds with the [mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins) kit. Check the
kit out next to this repo as a sibling named `mpc-vst`, then from this repo's root:

```sh
# offline x86 host test (ASan) — always run first
../mpc-vst/tools/test_port.sh vst/vst.json

# build the ARM .so + skin for the device (needs Docker)
vst/build.sh
```

`vst/build.sh` finds the kit at `../../mpc-vst` (override with `MPC_VST=...`). Then follow the kit's
`docs/RELEASING.md` to package the release zip + installer. The compiled `.so` is device-specific and
is **not** checked in.

### Devices with a read-only `/sdcard` or an exfat (noexec) SD card

Some modded units (certain Hakai / MockbaMod MPC One setups) mount `/sdcard` read-only and only read
plugin skins from the exfat SD card (`/media/MPCONE/Synths`, mounted `noexec`) and the firmware. On
those the portable installer can't place an executable `.so` where MPC would load it, so install by
hand with a **split layout** instead:

1. Copy `portable/nachtaktiv303 - VST - Vibe FX/Plugin Skins/` (and `version.xml`) to the SD `Synths`
   folder, e.g. `/media/MPCONE/Synths/nachtaktiv303 - VST - Vibe FX/`.
2. Copy `mpc_fx.so` to an **executable** path such as `/data/vst/`.
3. Add this line inside `<VALUE name="pluginList-arm"><KNOWNPLUGINS>` in `MPC.settings` (MPC stopped),
   with `file=` pointing at the executable `.so`:
   `<PLUGIN name="Vibe FX" descriptiveName="Vibe FX" format="VST" category="Effect" manufacturer="nachtaktiv303" version="1.0" file="/data/vst/mpc_fx.so" uid="4d744678" isInstrument="0" numInputs="2" numOutputs="2" isShell="0"/>`
4. Restart MPC. (A different SD card doesn't change this — exfat is always `noexec`; it's the device's
   storage layout, not the card.)

## Credits & licence

All DSP is first-party code — see [`src/VENDORED.md`](src/VENDORED.md). The algorithms gratefully
build on prior art, credited there: a Dattorro-derived FDN reverb, an Airwindows-inspired tape delay,
a Roland-Juno-style chorus and a tanh saturation. No third-party code is included.

Port, MPC integration and skin: **nachtaktiv303**. Released under the MIT License — see
[`LICENSE`](LICENSE). Unofficial and non-commercial.
