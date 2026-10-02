#!/usr/bin/env bash
# Build Vibe FX as an MPC OS VST2 insert effect with mpc-vst-plugins' generic port builder (vst.json).
#   vst/build/mpc_fx.so            -> /sdcard/vst/ on the device
#   vst/build/skin/<folder>/       -> /sdcard/Synths/ on the device
#   vst/build/pluginlist-entry.xml the <PLUGIN> line for MPC.settings' pluginList-arm
# Needs an mpc-vst-plugins checkout (MPC_VST) next to mpc-vst-vibe-fx.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
MPC_VST="${MPC_VST:-$here/../../mpc-vst}"
[ -x "$MPC_VST/tools/build_port.sh" ] || { echo "need an mpc-vst-plugins checkout (MPC_VST)" >&2; exit 1; }
exec "$MPC_VST/tools/build_port.sh" "$here/vst.json"
