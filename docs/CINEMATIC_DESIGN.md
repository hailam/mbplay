# Cinematic Mode — Design

Goal: an autopilot that zooms smoothly and indefinitely into interesting
locations, started from the viewer (hotkey) or via a CLI argument. Smoothness
comes from rendering *ahead* of the camera, not from rendering faster.

## Why per-frame rendering cannot work

The interactive deep path recomputes the viewport whenever the view changes.
A cinematic zoom at ~0.5–2 decades/second changes the view every frame; even
at ~1–2 ms/tile (post-BLA) the recompute + orbit handoffs cannot hold 60 fps,
and never will at extreme iteration budgets. Every production fractal-zoom
renderer (Kalles Fraktaler zoom sequences, zoomasm) solves this the same way:

## Keyframe pipeline (the "ahead tile render")

- Render **keyframes**: full offscreen viewport renders at exponentially
  spaced zooms, one per zoom doubling (`zoom_k = 2^k` along the dive path),
  centered on the dive target at that depth.
- Playback at zoom `z` with `2^k <= z < 2^(k+1)` composites:
  1. keyframe `k` upscaled by `z / 2^k` (in `[1, 2)`) over the whole frame;
  2. keyframe `k+1` downscaled by `z / 2^(k+1)` (in `[0.5, 1)`) as the sharp
     central inset (its complex extent maps to the middle of the screen).
  Both are exactly the reprojection math already used by
  `drawDeepPlaceholder` (center-delta via `mb_view_hp_center_delta_fx`,
  ratio via the fx scales) — including compensation if the dive path drifted
  between keyframes.
- A background worker keeps N≈4 keyframes rendered ahead. If the pipeline
  falls behind (next keyframe missing), *ease the zoom speed down* instead of
  stuttering; speed back up when the buffer refills.

### Cost check (why this holds 60 fps indefinitely)

At 0.5 dec/s ≈ 1.7 doublings/s ≈ 1.7 keyframes/s. One deep keyframe
(1280×800) is ~15–20 tiles × 1–2 ms (BLA) plus an orbit *continuation*
(same center, deeper budget: 2–50 ms — never a fresh multi-second build).
That is ≥10× real-time margin even at 10^4000. Compositing per frame is two
nearest/bilinear blits — trivial.

Use a **dedicated `MBDeepRenderer` instance** for keyframes: the interactive
renderer's generation counter aborts anything that is not the newest view,
which is exactly wrong for prefetching future views. For the cinematic
renderer, generation = keyframe index (monotonic), so orbit continuation
kicks in naturally as the dive deepens on a fixed-ish center.

## Choosing "interesting places" indefinitely

Boundary-following via iteration-count probes:

- Before scheduling keyframe `k+2`, render a cheap probe of the target
  neighborhood at depth `2^(k+2)`: the existing
  `mb_deep_renderer_render_tile_strided` gives a 16×16 sample for ~1/256th
  of a tile's cost (use raw iteration counts; add an iterations-out variant
  or a tiny probe entry point).
- Score pixels: highest *finite* (escaping) iteration count = closest to the
  boundary = spirals/minibrot cascades; weight toward the current center to
  avoid jerky retargets. Steer the dive center toward the winner with an
  eased HP translate (`mb_view_hp_translate_fx`), capped at ~¼ viewport per
  doubling so adjacent keyframes always overlap for compositing.
- This self-corrects forever: the boundary is infinitely deep everywhere, so
  the target never goes interior/exterior-flat. At the 10^4000 ceiling,
  restart from zoom 1 at a fresh seed (rotate through presets + a hash of
  the time) — "indefinite" as a loop.

## Entry points

- Viewer: hotkey (e.g. `V`) toggles cinematic mode; ESC drops back to
  interactive **at the current location** (a genuinely nice property — you
  can stop the movie and explore).
- CLI: `mandelbrot_interactive --cinematic [--speed 0.5] [--seed RE,IM]`
  parsed in `main_interactive.c`, passed through `native_viewer_init`.

## Implementation shape

- `src/cinematic/director.{c,h}` — pure C, testable: keyframe ring buffer
  (≈6 × viewport buffers ≈ 18 MB), dive-path state (HP center strings +
  zoom_log10), probe/retarget logic, prefetch scheduling decisions.
- Viewer glue in `native_viewer.m`: mode flag; in cinematic mode the display
  timer advances `zoom_log10` by `speed·dt`, asks the director for the two
  bracketing keyframes, composites, and draws a minimal HUD; input handler
  for enter/exit.
- Reuse: `MBDeepRenderer` (second instance), strided renders for probes,
  orbit continuation, fx reprojection math, HP center ops.

Estimated ~600–800 lines. No new algorithmic risk — every hard primitive
already exists and is tested; the new work is orchestration + compositing.

## Open questions / v2

- Interest heuristic tuning (avoid "boring" straight valleys): could add
  period detection to aim at minibrot chains explicitly.
- Ring blending between the two keyframes (zoomasm-style) instead of a hard
  inset edge; barely visible with nearest compositing at ≤2× scale, but easy
  polish later.
- Recording to video (the keyframe pipeline is exactly what an exporter
  needs — write keyframes as PNG + a tiny compositor and you get zoomasm
  input for free).
