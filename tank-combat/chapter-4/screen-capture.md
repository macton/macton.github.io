# Capturing the WebGPU page headlessly

How to render this chapter's WebGPU page and get a PNG out of it, from a headless
agent/CI box with no display and no real GPU. Written so the next agent doesn't have
to rediscover any of it.

## The one thing that will waste your afternoon

**You cannot screenshot a WebGPU canvas headlessly.** `page.screenshot()` /
`element.screenshot()` on the `#gpu` canvas returns **blank white** — the WebGPU
swapchain is GPU-backed and isn't in the compositor path Playwright captures under
`--headless`. This is *not* a sign your render failed; the page can be rendering
perfectly and the screenshot is still blank.

So there are two distinct jobs, and you pick the harness by which you need:

| Goal | How | Sees pixels? |
|---|---|---|
| **Visual capture** (a PNG of the scene) | Render to an **offscreen texture**, copy it to a buffer, read the buffer back, encode a PNG in Node | **Yes** |
| **Boot / validation check** (does it load, any GPU errors?) | Load the **real page**, drive its UI, watch `console`/`pageerror` | No — canvas stays blank |

Never try to make the first job work by screenshotting the canvas. Render offscreen
and read back.

## Environment

- Chromium is pre-installed; **do not** run `playwright install`. Find the binary:
  ```sh
  ls /opt/pw-browsers           # e.g. chromium-1194/chrome-linux/chrome
  ```
  Use that path as `executablePath` (the version dir changes — don't hardcode blindly).
- `playwright-core` is the driver. If it isn't already in a `node_modules` you can
  reach, `npm i playwright-core` in a scratch dir (with `PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD=1`
  so it doesn't try to fetch a browser).
- There is **no real GPU**: WebGPU runs on SwiftShader (software). It's correct but
  slow-ish; keep render sizes modest (1400×900 is fine).
- Work out of the session scratchpad, not the repo.

### Launch args that actually enable WebGPU on SwiftShader

```js
const browser = await chromium.launch({
  executablePath: "/opt/pw-browsers/chromium-1194/chrome-linux/chrome",  // from `ls /opt/pw-browsers`
  headless: true,
  args: ["--no-sandbox", "--headless=new", "--enable-unsafe-webgpu",
         "--enable-unsafe-swiftshader", "--use-angle=swiftshader", "--ignore-gpu-blocklist"],
});
```

If `navigator.gpu.requestAdapter()` returns null, an arg is missing — usually
`--enable-unsafe-swiftshader` or `--enable-unsafe-webgpu`.

## Visual capture: render offscreen → read back → PNG

The recipe (this is exactly what the session's `deferred-check.mjs` did):

1. Spin a tiny HTTP server. It can return the **same minimal HTML for every
   request** — you don't load the real page; you inject everything into the page
   context yourself. (Loading the real `app.js` is harder to instrument; injecting
   is simpler and lets you force a camera/LOD/etc.)
2. In `page.evaluate`, pass the real inputs in as **base64**:
   - `game.wasm` → instantiate it, `init()`, `tick()` a few times to get live state.
   - The **WGSL** — grab it straight out of `app.js` so you test the real shaders,
     don't re-type them (see "Grab the real WGSL" below).
   - Any texture assets (e.g. `town_atlas.png`) → `createImageBitmap(new Blob([bytes]))`.
     The dev server returns HTML for every path, so you can't `fetch` the asset — pass
     its bytes in.
3. Build the pipelines/bind groups, mirroring `app.js`. Render your passes into
   **offscreen textures** (G-buffer → lighting → tonemap → a final `rgba8unorm`
   "swap" texture). Nothing touches a canvas.
4. **Copy the final texture to a buffer and map it back**, then hand the bytes to Node
   and write a PNG.

### Reading a texture back (the part with the sharp edges)

`copyTextureToBuffer` requires `bytesPerRow` to be a **multiple of 256**. So you pad,
copy, map, then strip the padding row by row:

```js
const bpr = Math.ceil(W * 4 / 256) * 256;                 // padded stride
const rb = d.createBuffer({ size: bpr * H, usage: 0x1 | 0x8 });   // COPY_SRC | MAP_READ ... (see usage table)
enc.copyTextureToBuffer({ texture: swap }, { buffer: rb, bytesPerRow: bpr }, [W, H]);
d.queue.submit([enc.finish()]);
await rb.mapAsync(1 /* GPUMapMode.READ */);
const padded = new Uint8Array(rb.getMappedRange());
const tight = new Uint8Array(W * H * 4);                  // strip the per-row padding
for (let y = 0; y < H; y++) tight.set(padded.subarray(y * bpr, y * bpr + W * 4), y * W * 4);
// return Array.from(tight) (or a dataURL) out of page.evaluate, then PNG-encode in Node.
```

The swap texture needs `RENDER_ATTACHMENT | COPY_SRC` usage. The readback buffer needs
`COPY_DST | MAP_READ`.

### GPUTextureUsage / GPUBufferUsage bit values (memorize these — they bite)

The numbers differ between **textures** and **buffers**, and `COPY_DST` is the classic
trap:

```
Texture usage:  COPY_SRC=0x01  COPY_DST=0x02  TEXTURE_BINDING=0x04  STORAGE_BINDING=0x08  RENDER_ATTACHMENT=0x10
Buffer  usage:  MAP_READ=0x01  MAP_WRITE=0x02  COPY_SRC=0x04  COPY_DST=0x08  ... STORAGE=0x80
```

A texture you `copyExternalImageToTexture` into (e.g. the atlas) needs **`COPY_DST`
= `0x02`** + `RENDER_ATTACHMENT` (`0x10`). Writing `0x08` there gives it
`STORAGE_BINDING` instead and Dawn complains *"Destination texture needs CopyDst and
RenderAttachment usage"*. (Cost me a render. `0x08` is `COPY_DST` only for **buffers**.)

### `layout: "auto"` lies about float textures

`rgba16float` (HDR) and `rgba32float` (world-position G-buffer) are **not filterable**.
With `layout: "auto"` the pipeline infers a *filtering* sampler/`float` texture for
them and you get a cryptic `[Invalid CommandBuffer]` at submit. Fix: declare the bind
group layouts **explicitly** with `sampleType: "unfilterable-float"` for those
textures, and read them with `textureLoad` (no sampler) rather than `textureSample`.

### `textureSample` needs uniform control flow

`textureSample(...)` inside an `if` (non-uniform control flow) fails to compile — it
needs derivatives. In a fragment shader that branches (e.g. the imposter sampling the
atlas only when a UV is valid), sample **unconditionally** with
`textureSampleLevel(tex, samp, uv, 0.0)` (no derivatives) and branch on the result.

### Grab the real WGSL from app.js

Don't paste shader source into the harness — read it out of `app.js` so you're testing
what ships:

```js
const appjs = readFileSync(ROOT + "/app.js", "utf8");
const grab = (name) => { const a = appjs.indexOf("`", appjs.indexOf("const " + name)) + 1;
                         return appjs.slice(a, appjs.indexOf("`", a)); };
const WGSL = grab("WGSL ="), WGSL_SCREEN = grab("WGSL_SCREEN");
```

You can also string-replace into the grabbed WGSL to A/B a constant (the session swept
the shadow dither amplitude this way) without editing `app.js`.

### Forcing scene state

`init()` then `tick()` ~140 times gives a populated world (tanks moving, swarm spread).
Drive everything else from harness args rather than the page's controls: camera
(centre/dist/pitch/yaw/fov), LOD level, light/shadow params. Mirror `app.js`'s math
(MVP build, per-screen LOD distance, the sun ortho light matrix) so the capture matches
the real page.

## Boot / validation check (the real page, no pixels)

When you only need "does it load and are there GPU errors?", serve the **real files**
and load the **real page**:

```js
const server = createServer((req, res) => {
  let p = req.url.split("?")[0]; if (p === "/") p = "/index.html";   // strip ?v= cache-buster
  const f = join(ROOT, p); if (!existsSync(f)) { res.writeHead(404); res.end(); return; }
  res.writeHead(200, { "content-type": TYPES[extname(f)] || "application/octet-stream" });
  res.end(readFileSync(f));
});
page.on("pageerror", e => errors.push(String(e)));
page.on("console", m => { if (m.type() === "error" && !/favicon/.test(m.text())) errors.push(m.text()); });
```

Then wait for a known-good selector (e.g. `#freecam.active`), optionally click through
the UI, and assert `errors` is empty (a favicon 404 is the only expected noise). This
exercises the **real** `app.js` pipeline creation, bind groups, buffer writes, and a
few rendered frames — any GPU **validation** error surfaces as an uncaptured-error
console message even though you can't see the canvas. It does **not** confirm the
picture is correct; only the offscreen-capture harness does that.

## Getting the PNG in front of a human (or yourself)

- **The agent's own context has a hard ~32 MB cap on the *cumulative* images in the
  conversation.** Every tool call re-sends the whole transcript, so once the images
  you've already `Read` sum to ~32 MB, *any* further image `Read` is rejected
  ("Request too large (max 32MB)") — even a tiny one, and even for non-image calls
  that just carry the history. You cannot retroactively drop images already in the
  conversation; they clear only when the context next compacts.
- **To let the user see a result, send it to their device, not your context:** use
  `SendUserFile` (full-res, doesn't count against the 32 MB cap). Do this by default
  for deliverable renders.
- **To view it yourself when you're near the cap, downscale first.** There's no
  ImageMagick/sharp here, but the bundled Chromium will resize via an `OffscreenCanvas`:

  ```js
  // shrink.mjs <in.png> <out.png> <scale>
  const out = await page.evaluate(async ({ b64, scale }) => {
    const img = await createImageBitmap(new Blob([Uint8Array.from(atob(b64), c => c.charCodeAt(0))]));
    const w = Math.round(img.width * scale), h = Math.round(img.height * scale);
    const cv = new OffscreenCanvas(w, h); cv.getContext("2d").drawImage(img, 0, 0, w, h);
    return Array.from(new Uint8Array(await (await cv.convertToBlob({ type: "image/png" })).arrayBuffer()));
  }, { b64: readFileSync(inp).toString("base64"), scale });
  ```

  Even so, prefer `SendUserFile` — downscaling only delays hitting the cap.

## Verifying things that aren't a still frame

Some effects aren't visible in one static capture (a transient bolt-impact buzz, an
animation). Don't fake it with screenshots — **verify the wiring numerically** instead:
instantiate `game.wasm`, poke the relevant exported state in linear memory, `tick()`,
and assert the read-backs (the session confirmed the wall-shake this way: inject a
shake via the exported pointers, check the offset math and that the sim decays it).
A pixel diff of "patched vs unpatched" offscreen renders also works when you need to
prove geometry moved without eyeballing it.

## TL;DR checklist

1. Render to an **offscreen texture**; never screenshot the WebGPU canvas.
2. Launch Chromium from `/opt/pw-browsers/...` with the SwiftShader/unsafe-WebGPU args.
3. Inject `game.wasm` + the WGSL grabbed from `app.js` + asset bytes as base64.
4. `copyTextureToBuffer` with `bytesPerRow` padded to 256; map, strip padding, PNG in Node.
5. Mind the usage bits (texture `COPY_DST=0x02`), explicit `unfilterable-float` layouts,
   and `textureSampleLevel` under branches.
6. Use the real-page harness only for boot/validation (blank canvas is expected there).
7. Deliver renders via `SendUserFile`; downscale before `Read`-ing them yourself; the
   32 MB image cap is cumulative and unclearable until compaction.
