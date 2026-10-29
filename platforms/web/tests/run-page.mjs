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
import { writeFile } from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';
import { createIsolatedServer } from '../shell/serve.mjs';
import { installOffscreenCanvasContext, launchChromium } from './browser.mjs';

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

const server = createIsolatedServer(buildDir);
await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
const base = `http://127.0.0.1:${server.address().port}/`;

const browser = await launchChromium();
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
    if (!expectLogs.length && frames >= 30 && sample && sample.colours >= 8) break;
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
