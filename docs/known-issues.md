# Known issues

## Both-eyes stereo: some effects differ between the eyes (open)

**Symptom.** In both-eyes mode (F5 default, F7 head tracking on), hit
shockwaves (when 2B hits machines and when she is hit) and the slashes of the
charged "sphere" attack look different in each eye. Seen at 60 fps. They
seemed to render only in the right eye (not certain).

**How both-eyes works.** `src/game/stereo_render.cpp` hooks the draw job
(`exe+0x756360`, Compat build) and runs it twice: left eye as recorded, then the
right eye after re-uploading camera-dependent constant buffers. Anything in the
render queue that behaves differently on its second execution breaks the right
eye.

**Fixed already (for reference):**

- Camera constant buffers mapped before the job (SceneBuffer 256 B, previous
  view*projection 80 B +0x10, camera position 32 B +0x10): re-uploaded with the
  right eye.
- Per-object light records (lock-free stack, pushed with tag `'L'` in
  `exe+0x214100`, drained by `exe+0x21C560`): the left pass emptied it, so the
  right pass drew no lights (Pod bullets lit in one eye only). The pop
  (`exe+0x21E940`) is hooked; the right pass replays the left pass's records.

**What the tools showed for the remaining issue (F10 node census, F4 on):**

- Both passes run the same commands and the same number of draws.
- Only one draw's inputs differ: command callback `exe+0x77B250` (tonemap /
  final pass), pixel shader texture **t5** = `DAT_14178f230`, the auto-exposure
  output of `exe+0x7777A0` (compute ping-pong). Explains slight exposure
  differences, not invisible effects.
- `exe+0x22D9D0` and `exe+0x747CF0` (-> `exe+0x21C3D0`) do 6
  `UpdateSubresource` calls in the left pass and 0 in the right. Path:
  `exe+0x74AEA0` -> `exe+0x275700(ctx, 0)` -> `exe+0x274F00`, which uploads only
  while `CameraData+0x28C` is set (cleared by the upload).
- Several commands do a `CopyResource`/`CopySubresourceRegion` in only one of
  the passes (callbacks `exe+0x77FAC0`, `exe+0x776280`, `exe+0x77F000`, and
  copies inside the big draw callbacks `exe+0x83D360/0x83D430/0x83D7E0`,
  `exe+0x2302D0`, `exe+0x7777A0`). Likely "copy the scene for distortion once
  per frame" style logic; distortion effects (shockwaves) would then sample a
  copy taken during the other pass.
- `exe+0x207B30` issues an occlusion query only in the left pass (draws match).

**Tried and not sufficient:**

- Re-sending in-job `UpdateSubresource` camera uploads with the right eye
  (commit `65f6de2`): no upload matched the left camera matrices, so the 6
  updates hold the camera in another form (or aren't camera data). Nothing was
  substituted. Dumping their contents next to CameraData is the next step.
- Snapshot/restore of UAVs and small render targets between passes (F3), and of
  full-size targets too (F2): no visible change; F3 stalls auto-exposure (bright
  areas stay overexposed), so it is off by default.

**Suggested next steps:**

1. Dump the 6 `UpdateSubresource` payloads (size, contents) from
   `exe+0x274F00` next to CameraData to see what they carry, then either force
   `CameraData+0x28C` before the right pass with right-eye matrices in place, or
   substitute the payload.
2. For the one-pass copies: log source/destination resources of each copy and
   which later draws sample the destination; replay the copies in the right pass
   (or force whatever "copied this frame" flag the callbacks check).
3. Verify with F4 (right pass with the left camera): both eyes must then be
   pixel-identical; any remaining difference is per-pass state, not camera data.

**Debug keys:** F2 full-size restore, F3 state restore, F4 same-camera test,
F10 node census (log lines `[nodes]`), F11 constant-buffer census.

## Game over screen shown in VR instead of theater (unverified fix)

The game over screen keeps camera mode 0 and the camera running. Commit
`65f6de2` switches to theater when the possessed entity's health is 0
(`src/game/player.cpp`; entity list `exe+0x101C430`, possessed handle
`exe+0x124315C` resolved on the Compat build). Not yet confirmed in game: no
`player down` line was logged in the first test. If health stays non-zero on
that screen, the F9 field diff found 22 candidate fields in
`cCameraGame+0x8308..0x839C` (death-camera preset) as an alternative signal.
