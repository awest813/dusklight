// Smoke test of a real web build: starts the game without a disc in headless Chromium and checks
// that it renders its prelaunch screen.
//
//   node platforms/web/tests/run-page.mjs <build-dir> [--seconds 60] [--frame out.png]
//        [--disc image.iso] [--expect-log <regex>]...
//
// With --disc, the image is attached through the page's picker ("use this file for now") instead
// of starting without a disc. With --expect-log, the test passes once every pattern has matched a
// line the game printed (whatever happens afterwards), instead of waiting for rendered frames.
//
// The build directory is what `cmake --build --preset web` produces (index.html, dusklight.js,
// .wasm, .data, ...). WebGPU runs on SwiftShader, so the game is started with
// --allow-software-gpu. Headless Chromium cannot allocate a WebGPU canvas swap chain on
// SwiftShader, so the page's canvas context is replaced with one that hands out ordinary textures
// on the same device: the game's whole render and present path still runs, only the browser's
// final compositing is skipped. The test passes when frames with visible content were presented
// and the page reported no fatal error.
import { createServer } from 'node:http';
import { readFile, writeFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import path from 'node:path';
import process from 'node:process';

const require = createRequire(import.meta.url);
const argv = process.argv.slice(2);
const buildDir = path.resolve(argv.shift() ?? '');
let seconds = 60;
let framePath = null;
let discPath = null;
const expectLogs = [];
while (argv.length) {
  const flag = argv.shift();
  if (flag === '--seconds') seconds = Number(argv.shift());
  else if (flag === '--frame') framePath = argv.shift();
  else if (flag === '--disc') discPath = path.resolve(argv.shift());
  else if (flag === '--expect-log') expectLogs.push(new RegExp(argv.shift()));
  else {
    console.error(`run-page: unknown argument ${flag}`);
    process.exit(2);
  }
}

const types = {
  '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css',
  '.wasm': 'application/wasm',
};
const server = createServer(async (req, res) => {
  res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
  res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
  let file = path.join(buildDir, path.normalize(decodeURIComponent(new URL(req.url, 'http://x').pathname)));
  if (file.endsWith(path.sep)) file = path.join(file, 'index.html');
  try {
    const data = await readFile(file);
    res.setHeader('Content-Type', types[path.extname(file)] ?? 'application/octet-stream');
    res.end(data);
  } catch {
    res.statusCode = 404;
    res.end();
  }
});
await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
const base = `http://127.0.0.1:${server.address().port}/`;

// Runs in the page before its scripts: see the header comment.
function installOffscreenCanvasContext() {
  const configs = new WeakMap();
  GPUCanvasContext.prototype.configure = function (config) {
    configs.set(this, { config, texture: null });
  };
  GPUCanvasContext.prototype.unconfigure = function () {
    configs.delete(this);
  };
  GPUCanvasContext.prototype.getConfiguration = function () {
    return configs.get(this)?.config ?? null;
  };
  GPUCanvasContext.prototype.getCurrentTexture = function () {
    const state = configs.get(this);
    if (!state) throw new DOMException('context is not configured', 'InvalidStateError');
    const { width, height } = this.canvas;
    if (!state.texture || state.texture.width !== width || state.texture.height !== height) {
      state.texture?.destroy();
      state.texture = state.config.device.createTexture({
        size: [width, height],
        format: state.config.format,
        usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC | GPUTextureUsage.TEXTURE_BINDING |
          GPUTextureUsage.COPY_DST,
      });
    }
    globalThis.duskTestFrames = (globalThis.duskTestFrames ?? 0) + 1;
    globalThis.duskTestLast = { texture: state.texture, device: state.config.device };
    return state.texture;
  };
  // How many distinct colours a sample of the last presented frame has.
  globalThis.duskTestSampleFrame = async () => {
    const last = globalThis.duskTestLast;
    if (!last) return null;
    const { texture, device } = last;
    const bytesPerRow = Math.ceil((texture.width * 4) / 256) * 256;
    const buffer = device.createBuffer({ size: bytesPerRow * texture.height, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    const encoder = device.createCommandEncoder();
    encoder.copyTextureToBuffer({ texture }, { buffer, bytesPerRow }, [texture.width, texture.height]);
    device.queue.submit([encoder.finish()]);
    await buffer.mapAsync(GPUMapMode.READ);
    const bytes = new Uint8Array(buffer.getMappedRange());
    const colours = new Set();
    for (let y = 0; y < texture.height; y += 7) {
      for (let x = 0; x < texture.width; x += 7) {
        const i = y * bytesPerRow + x * 4;
        colours.add((bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2]);
      }
    }
    buffer.unmap();
    buffer.destroy();
    return { colours: colours.size, width: texture.width, height: texture.height };
  };
  // The last presented frame as a PNG data URL.
  globalThis.duskTestFramePng = async () => {
    const last = globalThis.duskTestLast;
    if (!last) return null;
    const { texture, device } = last;
    const bytesPerRow = Math.ceil((texture.width * 4) / 256) * 256;
    const buffer = device.createBuffer({ size: bytesPerRow * texture.height, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    const encoder = device.createCommandEncoder();
    encoder.copyTextureToBuffer({ texture }, { buffer, bytesPerRow }, [texture.width, texture.height]);
    device.queue.submit([encoder.finish()]);
    await buffer.mapAsync(GPUMapMode.READ);
    const src = new Uint8Array(buffer.getMappedRange());
    const image = new ImageData(texture.width, texture.height);
    const bgra = texture.format.startsWith('bgra');
    for (let y = 0; y < texture.height; ++y) {
      for (let x = 0; x < texture.width; ++x) {
        const i = y * bytesPerRow + x * 4;
        const o = (y * texture.width + x) * 4;
        image.data[o] = src[i + (bgra ? 2 : 0)];
        image.data[o + 1] = src[i + 1];
        image.data[o + 2] = src[i + (bgra ? 0 : 2)];
        image.data[o + 3] = 255;
      }
    }
    buffer.unmap();
    buffer.destroy();
    const canvas = new OffscreenCanvas(texture.width, texture.height);
    canvas.getContext('2d').putImageData(image, 0, 0);
    const blob = await canvas.convertToBlob({ type: 'image/png' });
    return await new Promise((resolve) => {
      const reader = new FileReader();
      reader.onload = () => resolve(reader.result);
      reader.readAsDataURL(blob);
    });
  };
}

const { chromium } = require('playwright');
const browser = await chromium.launch({
  args: ['--enable-unsafe-webgpu', '--enable-unsafe-swiftshader', '--use-webgpu-adapter=swiftshader'],
});
let failure = null;
try {
  const page = await browser.newPage({ viewport: { width: 960, height: 720 } });
  await page.addInitScript(installOffscreenCanvasContext);
  const lines = [];
  page.on('console', (m) => {
    lines.push(m.text());
    console.log(`[page] ${m.text()}`);
  });
  page.on('pageerror', (e) => console.log(`[page error] ${e.stack ?? e.message}`));
  if (discPath) {
    await page.goto(`${base}?arg=--allow-software-gpu`);
    await page.waitForSelector('#disc:not([hidden])');
    await page.setInputFiles('#file', discPath);
    await page.waitForSelector('#picked:not([hidden]), #disc-error:not([hidden])');
    if (await page.isVisible('#disc-error')) {
      throw new Error(`the page rejected the disc: ${await page.textContent('#disc-error')}`);
    }
    await page.check('input[value=session]');
    await page.click('#play-picked');
  } else {
    await page.goto(`${base}?nodisc&arg=--allow-software-gpu`);
  }
  const unmatched = () => expectLogs.filter((re) => !lines.some((line) => re.test(line)));

  const deadline = Date.now() + seconds * 1000;
  let sample = null;
  while (Date.now() < deadline) {
    await page.waitForTimeout(2000);
    if (expectLogs.length && unmatched().length === 0) break;
    const fatal = await page.evaluate(() => window.duskState?.fatal ?? null);
    if (fatal !== null) {
      failure = `the page reported a fatal error: ${fatal}`;
      break;
    }
    const frames = await page.evaluate(() => globalThis.duskTestFrames ?? 0);
    sample = await page.evaluate(() => globalThis.duskTestSampleFrame?.());
    console.log(`run-page: ${frames} frames presented; last frame ${JSON.stringify(sample)}`);
    if (frames >= 30 && sample && sample.colours >= 8) break;
  }
  if (expectLogs.length) {
    if (unmatched().length === 0) {
      failure = null;
    } else {
      failure ??= `never logged: ${unmatched().map(String).join(', ')}`;
    }
  } else if (!failure && !(sample && sample.colours >= 8)) {
    failure = 'no frame with visible content was presented in time';
  }
  if (framePath) {
    const png = await page.evaluate(() => globalThis.duskTestFramePng?.());
    if (png) await writeFile(framePath, Buffer.from(png.split(',')[1], 'base64'));
  }
} finally {
  await browser.close();
  server.close();
}

if (failure) {
  console.error(`run-page: FAIL: ${failure}`);
  process.exit(1);
}
console.log(expectLogs.length ? 'run-page: every expected log line appeared' :
  'run-page: the game rendered its prelaunch screen');
