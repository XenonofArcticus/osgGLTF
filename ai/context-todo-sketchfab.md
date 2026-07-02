# Sketchfab Visual Parity — Planning Notes

## Status: ACTIVE

> **Last updated:** 2026-07-02
> **Repo(s):** osgGLTF | OpenSceneGraph.py

**Goal: match Sketchfab's render quality, first and foremost** — not just "not
broken." Comparing our glTF PBR/IBL render against Sketchfab's own web viewer
(using `~/tmp/3dmodels/deadspace00`/`deadspace01`, two Sketchfab-sourced "Dead
Space" models pulled in for the `osgSlug` HUD demo) to close the visual gap
between our render and theirs. Base material correctness is now close after
this session's loader fixes (spec-gloss/UV/factor propagation, then sRGB —
sRGB fix verified 2026-07-02 against BoomBox vs. BabylonJS's glTF renderer,
essentially a match); the remaining gap splits into two buckets: real
spec-gloss BRDF support (a real fidelity gap on extension-only materials like
`deadspace00`), and their "Final Render" post-processing stack (a feature we
don't have yet, bigger effort).

---

## Background

Same-session work fixed three loader bugs (`KHR_materials_pbrSpecularGlossiness`
not parsed, `texCoord`-aware UV binding, `metallicFactor`/`roughnessFactor` not
propagated — see osgGLTF project memory `gltf_pbr_factor_uv_fix` for full
detail) that made `deadspace00`/`deadspace01` render close to correct. Comparing
side-by-side against Sketchfab's viewer with "No Post-Processing" enabled (their
raw PBR output, no grading/bloom/etc.), our render is now genuinely close on
material rendering itself. Remaining differences:

1. ~~We don't do sRGB decode on `baseColorTexture`/`emissiveTexture`~~ — FIXED
   2026-07-02, see "sRGB (DONE)" below.
2. Sketchfab's studio HDRI/lighting setup is probably just better-tuned than
   whatever `--hdr`/`--ktx2` we're currently feeding `09-ibl.py` — not a bug,
   an environment/asset-quality difference. Note: since the sRGB fix makes
   `albedo` correctly darker, `09-ibl.py`'s `ibl_diff` term (which multiplies
   by `albedo` and `iblIntensity`) will read dimmer than before for the same
   `--ibl-intensity` — may be worth retuning that flag now; `ibl_spec` isn't
   scaled by `iblIntensity` at all currently, so reflections are unaffected
   either way.
3. Sketchfab's "Final Render" mode (their published post-processing feature)
   adds a real-time post stack we don't have any equivalent of yet.

---

## Current State

| Phase | Description | Status |
|---|---|---|
| 1 | Loader: spec-gloss extension, multi-UV texCoord binding, metallicFactor/roughnessFactor propagation | ✓ COMPLETE |
| 2 | sRGB color space handling for baseColor/emissive textures | ✓ COMPLETE — verified against BabylonJS reference (BoomBox), 2026-07-02 |
| 3 | Post-processing stack (tonemap/grade, bloom, vignette, SSAO, AA) | PENDING |
| 4 | Real spec-gloss BRDF support (proper conversion or shader branch) | PENDING |

---

## Open Work

### sRGB (DONE — 2026-07-02)

`src/GLTFReader.h`'s `getOrCreateTexture()` used to always set
`GL_RGB8`/`GL_RGBA8` as the internal texture format regardless of which
material channel a texture fed. Per the glTF spec, `baseColorTexture` and
`emissiveTexture` are authored in sRGB gamma space; `normalTexture`/
`metallicRoughnessTexture`/occlusion are linear. Every texture was being
treated as linear on sample, so the shader did lighting math on gamma-encoded
values directly — then `09-ibl.py` applied its own `pow(color, 1/2.2)` on top
at the end, compounding the error.

**Fix landed:** `getOrCreateTexture(int texIdx, bool sRGB)` now takes a
color-space flag; `applyMaterial()`'s `bindTexture` lambda passes `true` for
base/diffuse and emissive, `false` for normal/MR/specGloss (the packed
specular-color+glossiness texture uses `GL_SRGB8_ALPHA8`, which conveniently
decodes RGB only and leaves alpha/glossiness untouched — correct for both
channels in one tag). Texture cache key now includes the sRGB/linear flag so
the same image file used in two different color-space roles wouldn't
incorrectly share a cached `Texture2D`. **No `09-ibl.py` shader changes were
needed** — `GL_SRGB8`/`GL_SRGB8_ALPHA8` decode is transparent to any shader
`texture()` call; the shader's existing final `pow(color, 1/2.2)` was already
correct (converts the composed linear result to display sRGB) and didn't need
touching.

**Verified:** `deadspace01` in `09-ibl.py` now visibly darker/more contrasty
with crisper specular glints (matches theory — gamma-encoded bytes read
brighter than true linear values). BoomBox compared directly against
BabylonJS's glTF renderer side-by-side — near-identical match.

### Post-processing stack (bigger, multi-session effort)

Sketchfab's "Final Render" toggle is their published post-processing feature:
tone mapping + exposure/color grading, bloom, vignette, an SSAO-style
contact-shadow pass, and anti-aliasing (FXAA/TAA or supersampling). This is
architecturally the same shape as `demo.py`'s existing multi-pass RTT pipeline
(scene→RTT, chained fullscreen passes, composite) — just a different composite
shader/stack at the end instead of the hologram HUD look. Natural next step
would be to bolt a bloom+vignette+grade pass onto `09-ibl.py`'s output rather
than build a new pipeline from scratch.

No design decisions made yet on ordering/scope of the post stack — this needs
its own planning pass when picked up.

### Real spec-gloss BRDF support (goal: match Sketchfab, not just "not broken")

Today's Phase 1 fix (`applyMaterial()` parsing `KHR_materials_pbrSpecularGlossiness`)
only kicks in `if (!haveCoreBaseColor)` — i.e. only when a material has *no*
core `pbrMetallicRoughness.baseColorTexture` at all. Checked all of the
Khronos sample set's `*/glTF-pbrSpecularGlossiness/*.gltf` variants (BoomBox,
Avocado, WaterBottle, Lantern) — every one of them bakes a core
`pbrMetallicRoughness.baseColorTexture` fallback *alongside* the extension in
the same material, which is the standard Khronos pattern for that folder. So
our loader never actually exercises the new extension-parsing code on any of
those files — it just uses the (already correct, already verified) core PBR
path, same as the plain `glTF/` folder. That's fine/expected, not a gap.

The real gap is `deadspace00`-shaped files: extension-only, no core fallback.
For those, our fix currently routes `specularGlossinessTexture` into the same
GL unit `09-ibl.py` treats as `ormTex` (R=AO, G=roughness, B=metallic) — but a
spec-gloss texture's actual channels are RGB=specular color (F0), A=glossiness.
Completely different semantics. The shader ends up computing metallic/roughness
from specular-color/glossiness bytes, i.e. garbage. This is very likely a
meaningful chunk of why `deadspace00` still doesn't fully match the Sketchfab
reference (whose own Model Inspector panel literally lists "Specular" and
"Glossiness" as separate material channels — confirming they're doing a real
spec-gloss-aware render, not silently converting to metal-rough on load).

Two ways to actually close this, need to pick one when scoped:
1. **Loader-side conversion** — convert spec-gloss (diffuse, specular, glossiness)
   into an approximate metallicFactor/roughnessFactor + baseColor at load time,
   using the standard published conversion heuristic from the (archived)
   `KHR_materials_pbrSpecularGlossiness` spec (roughness ≈ 1 - glossiness;
   metalness estimated from how close the specular color is to a pure
   dielectric ~0.04 vs. tinted-metal reflectance). Keeps `09-ibl.py`'s shader
   untouched; loses some fidelity vs. true spec-gloss (max/blend/albedo
   requires an actual formula, not just "channel A here").
2. **Shader-side dual path** — add a `KHR_materials_pbrSpecularGlossiness`
   branch to `09-ibl.py`'s fragment shader that computes the Cook-Torrance
   terms directly from specular color (as F0) and glossiness (as inverse
   roughness), no conversion needed, more accurate — but requires a per-material
   "which workflow" flag/uniform threaded through from the loader, and doubles
   the BRDF code path to maintain.

Given the stated goal ("match Sketchfab first and foremost"), (2) is probably
the more faithful option long-term since it avoids lossy conversion, but (1) is
much less work and may be "good enough" — needs a real decision, not a default,
when this is picked up.

**Concrete repro cases** (found 2026-07-02, both point at the same channel
mismatch above — use these to confirm the fix when it lands):

1. **"Wet/rubber" look under `--animated-lights`.** Rotate a light around
   `deadspace00` in `09-ibl.py` and the whole suit reads as one uniform glossy
   material sliding smoothly under the highlight — no per-part variation
   between armor plates, fabric straps, visor glass. Root cause: `roughness`/
   `metallic` are currently sampled from the specular-color RGB channels
   (fairly uniform across the texture) instead of the real glossiness channel
   (the specGloss texture's alpha, which presumably *does* vary by suit part
   and isn't read anywhere at all right now). A correct BRDF is already
   running (Cook-Torrance, same as the metallic-roughness path) — it's just
   being fed near-constant garbage roughness, which is exactly what produces
   a "molded from one material" look.
2. **Crushed-dark legs/boots with `--no-lights --ibl-intensity 0.5`.** With
   direct lights off, ambient IBL is the only light source, so some darkening
   in occluded areas vs. Sketchfab's viewer is expected at low intensity. But
   `ao = texture(ormTex, vUV).r` is, for this material, really the **red
   channel of the specular color**, not baked occlusion — if that channel
   happens to read low in the leg/boot region, `ambient = (ibl_diff + ibl_spec)
   * ao` gets crushed further on top of the already-dim ambient-only exposure.
   Expect this to *partially* improve once real spec-gloss support removes the
   bogus AO read, though some darkening here is just an inherent consequence
   of no-direct-light + low intensity, not a bug.

---

## Key Files

- `src/GLTFReader.h` — `getOrCreateTexture()` / `applyMaterial()`'s `bindTexture`
  lambda; sRGB channel-awareness landed here 2026-07-02
- `src/ReaderWriterKTX2.cpp` — has the original `GL_SRGB8_ALPHA8` precedent for
  the environment cubemap that the 2D-texture fix followed
- `~/dev/OpenSceneGraph.py/examples/pyosg-lighting/09-ibl.py` — the working
  PBR+IBL shader; needed no changes for the sRGB fix (GPU decode is
  shader-transparent), but its `ibl_diff`/`iblIntensity` interaction is worth
  retuning now that `albedo` reads correctly darker (see item 2 above)
- `demo.py` (osgGLTF repo root) — existing multi-pass RTT architecture to model
  a post-processing stack after
- `ext/glTF-Sample-Models/2.0/*/glTF-pbrSpecularGlossiness/*.gltf` — Khronos's
  own spec-gloss test variants; all confirmed to carry a core PBR fallback
  alongside the extension, so they don't exercise the extension-only code path
  and aren't useful for testing the real spec-gloss BRDF gap. Use
  `~/tmp/3dmodels/deadspace00/scene.gltf` (extension-only, no fallback) instead.

---

## Notes / Constraints

- Don't conflate "material correctness" (sRGB, factors, UV — bugs, should just
  be fixed) with "post-processing" (a feature we don't have, not a bug — no
  urgency, scope it deliberately when picked up).
- Sketchfab's lighting-rig quality (HDRI choice/tuning) isn't something we can
  "fix" in code — closing that gap is an asset/tuning question, not a shader bug.
