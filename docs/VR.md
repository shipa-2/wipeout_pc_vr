# VR

WipEout Omega Collection has a PlayStation VR mode. The port runs it in a PC headset: the game's
PS VR libraries (`libSceHmd`, `libSceVrTracker`, `libSceCamera`, from
[AstroQuest](https://github.com/bigmak94/AstroQuest), its shadPS4 fork) talk to an OpenXR runtime
instead of a PS VR, so a headset SteamVR drives should work: Valve Index, Meta Quest through Steam
Link, Air Link, ALVR or Virtual Desktop, and others. Tested: Meta Quest 2 through SteamVR (ALVR) on
Linux.

VR is off unless it is turned on: the game sees no headset and plays as before.

## Starting

1. Start SteamVR (or another OpenXR runtime) with the headset connected.
2. Start the game with `SHADPS4_VR=1` in the environment of `run.py`. The game then draws
   60 frames a second, which the headset shows twice each, with its own dynamic resolution.

The window shows the left eye. Do not kill the game process (`kill -9`, stopping it in a debugger)
while SteamVR shows it: SteamVR may crash with it.

## Environment

| Variable | Meaning |
|---|---|
| `SHADPS4_VR=1` / `0` | Report a connected PS VR to the game / never (the default) |
| `SHADPS4_OPENXR=0` | Do not use OpenXR. With `SHADPS4_VR=1` the game runs its VR mode with the left eye in the window only: for testing without a headset |
| `SHADPS4_VR_FOV=<percent>` | How much of the headset's field of view the game is told it has |
| `SHADPS4_VR_FOV_OF=psvr` | Tell the game a PS VR's field of view (100°×103° an eye) instead of the headset's own |
| `SHADPS4_XR_PREDICT_MS` | How far ahead the head pose is predicted for a frame (20) |
| `SHADPS4_VR_PHASE_LOCK=0` | Headset refreshes for the game on a clock of their own, not the runtime's |
| `SHADPS4_XR_END_LOCK=1` | `xrEndFrame` under the queue lock (SteamVR 2.18 waits there for the display: 13 FPS) |
| `GOW3_EVF_DEFER=0` | The VR tracker's event flag set at once (a lost wakeup froze the VR menu) |

`vr.json` in the user folder: `"headset": "on"` / `"off"`, the IPD (`ipd_mm`) and the controller's
place (`pad_offset`).

## How it works

- `core/libraries/hmd`, `vr_tracker`, `camera`: the PS VR libraries. The reprojection's flips
  (`Hmd::OnVblank`) come at the headset's refresh rate, phase-locked to the runtime's.
- `core/vr/vr_runtime`: the emulated headset (pose, field of view, refresh rate);
  `core/vr/openxr_host`: the OpenXR session, its swapchain and the head and hand poses.
- `VideoOutDriver::SubmitHmdFrame`: a frame the game hands the reprojection goes to the GPU
  thread in order with the draws that made it; `Presenter::PresentHmdFrame` copies both eyes
  (layers of one array texture) into the OpenXR swapchain image, which the host shows once the GPU
  is done.
- The headset the game is told of has square pixels: its panel as wide as the headset's field of
  view asks, the eyes at most 4096 pixels wide (`SHADPS4_VR_ASPECT=psvr`: a PS VR's 1920×1080).
- The game's runtime services: `sceSystemServiceReceiveEvent` hands the game the runtime's
  events ("reset the VR position").
