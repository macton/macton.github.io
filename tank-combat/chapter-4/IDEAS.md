# Chapter 4 — ideas to review later

A running list of rendering ideas worth prototyping but deliberately deferred. Nothing here
is committed to the pipeline yet; each is an A/B candidate behind a toggle.

---

## 1. GTAO — ground-truth ambient occlusion (quality add)

Source: Jimenez et al., *Practical Realtime Strategies for Accurate Indirect Occlusion*
(Activision, ATVI-TR-16-01). 0.5 ms on PS4 @ 1080p **with full temporal pipeline**.

**Why it's interesting for us:** our shading has **no ambient occlusion at all** — the ambient
term is a flat hemisphere lerp (`mix(ground, sky, n.z*0.5+0.5)` in `fs_light`), so a crevice, a
corner, the gap under a tank, and an open plaza all receive the *same* ambient. That flat ambient
is the single biggest reason objects read a little floaty in shadow. Our existing "screen-space
shadow" (`sun_shadow`) is a **directional** sun contact shadow — a different effect; it does not
ground objects in the ambient term.

**What GTAO is:** per-pixel hemispherical AO via screen-space **horizon search** + an **analytic
cosine-weighted** inner integral between the two horizon angles (the "ground truth" part). Made
cheap by distributing samples over space+time: 1 dir/pixel × 4×4 bilateral spatial × 6 temporally
reprojected rotations = 96 effective dirs, at half-res.

**Fit / friction:**
- Good: we already have the deferred inputs (depth + world normal) and already march in screen
  space, so the horizon search is a natural extension; flat-shaded low-poly takes AO very well.
- Friction: GTAO's cheapness leans on **temporal reprojection**, and we have **no temporal infra**
  (no motion vectors, no history; camera + sim-interp move every frame). Full GTAO = building
  TAA-style plumbing.
- It's a **quality add, not a perf win** — we're fill-bound on mobile, so any AO costs frames.

**Recommended lightweight path (the actual TODO):** a **half-res, spatial-only horizon AO** folded
into the `ambient` term (reuse the march infra + the SSS buffer slot + a bilateral upsample), ~80%
of the look without temporal plumbing. Steal two cheap tricks regardless: the **thickness
heuristic** (stops our thin tree trunks / tank barrels over-occluding) and the **AO→GI cubic remap**
(recovers near-field bounce so corners don't go muddy). Skip **GTSO** (specular occlusion) — we're
diffuse-only. Put it behind a toggle and A/B cost-vs-grounding on-device.

---

## 2. Shrink the G-buffer (bandwidth wins)

Context (from the r/opengl "reducing the size of the g-buffer" thread — couldn't fetch it directly,
Reddit blocks this sandbox; these are the standard techniques such a thread covers, applied to our
buffer). We're **fill/bandwidth-bound** on mobile, and the deferred passes re-read the G-buffer
every full-screen pass, so per-pixel byte count is a direct lever.

**Our current strict G-buffer = 12 B/px:**
- `gColorTex` rgba8unorm (4) — albedo.rgb + a = sky mask (`a < 0.5` ⇒ sky)
- `gNormalTex` rgba8unorm (4) — world normal `*0.5+0.5`, only 3 of 4 channels used
- `depthTex` depth32float (4) — used to reconstruct world position (the fat rgba32f position
  target is already gone — that's the #1 classic win, already banked)

Candidates, cheapest/safest first:

- [x] **Octahedral-encode the normal: `gNormal` rgba8 → rg8** (4 → 2 B/px). **DONE** (v…104533) —
      `oct_encode`/`oct_decode` (Cigolle et al.) in both shader modules; verified round-trip 0.95°
      worst-case with rg8 quantization. Strict G-buffer 12 → 10 B/px. The freed b/a channels are not
      yet reclaimed (could still carry a material id / move the sky mask off `gColor.a`).
- [ ] **Reconstruct the normal from depth derivatives** (eliminate `gNormalTex` entirely, save the
      whole 4 B/px + its write in geom + its read in every lighting pass). For our **flat-shaded
      faces** the depth-derivative *is* the face normal in interiors — exactly what we want — but it
      spikes to garbage at depth discontinuities (silhouette pixels). Mitigate with the smaller of
      ddx/ddy, or accept a 1px edge artifact in a stylized look. Biggest single win; highest risk.
      Strict G-buffer would drop **12 → 8 B/px** (33%).
- [ ] **Move the sky mask off `gColor.a`** (use cleared depth == 1.0 as "sky", which we already
      effectively have), reclaiming the alpha channel for packing (only pays off combined with the
      octahedral-normal packing above — no smaller renderable RGB format exists to shrink `gColor`
      alone).
- [ ] **HDR rg11b10ufloat** (8 → 4 B/px) — **already done** (with rgba16float fallback). Listed for
      completeness; it's the lighting target, not strictly the G-buffer.
- [ ] **depth32float → depth24** — not worth it: we sample depth for position reconstruction and
      want the precision; the 1 B/px saving isn't renderable as a sampled color anyway.
- [ ] **Half-resolution G-buffer / passes for cheap effects** (AO, point-light volumes) — pairs
      naturally with the GTAO idea above; AO especially does not need full res.

**Most promising to prototype:** octahedral normal (safe, −2 B/px) first; then test
normal-from-depth (−4 B/px, risk at edges) as a bigger swing. Both behind a toggle, A/B on-device,
watch the `light`/`plights`/`fx`/`tone` pass times in the config sweep.
