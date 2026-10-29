// Profiles the browser main thread of a web build for a while and prints where its time goes:
//
//   node platforms/web/tests/profile-page.mjs <build-dir> [--seconds 20] [--disc image.iso]
//        [--top 30] [--save profile.cpuprofile]
//
// Starts the game like run-page.mjs (headless Chromium, WebGPU on SwiftShader, --allow-software-gpu)
// and records a CPU profile of the page's main thread (the game loop, aurora and the WebGPU calls;
// the GPU's own work runs elsewhere). Prints the functions with the most self time, the share of
// idle time (waiting for the next animation frame), and frames presented per second. A saved
// .cpuprofile opens in Chrome's DevTools.
import { writeFile } from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';
import { createIsolatedServer } from '../shell/serve.mjs';
import { installOffscreenCanvasContext, launchChromium } from './browser.mjs';

const argv = process.argv.slice(2);
const buildDir = path.resolve(argv.shift() ?? '');
let seconds = 20;
let discPath = null;
let top = 30;
let savePath = null;
while (argv.length) {
  const flag = argv.shift();
  if (flag === '--seconds') seconds = Number(argv.shift());
  else if (flag === '--disc') discPath = path.resolve(argv.shift());
  else if (flag === '--top') top = Number(argv.shift());
  else if (flag === '--save') savePath = argv.shift();
  else {
    console.error(`profile-page: unknown argument ${flag}`);
    process.exit(2);
  }
}

const server = createIsolatedServer(buildDir);
await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
const base = `http://127.0.0.1:${server.address().port}/`;
const browser = await launchChromium();
try {
  const page = await browser.newPage({ viewport: { width: 960, height: 720 } });
  // Counts presented frames in globalThis.duskTestFrames.
  await page.addInitScript(installOffscreenCanvasContext);
  const lines = [];
  page.on('console', (m) => lines.push(m.text()));
  if (discPath) {
    await page.goto(`${base}?arg=--allow-software-gpu`);
    await page.waitForSelector('#disc:not([hidden])');
    await page.setInputFiles('#file', discPath);
    await page.waitForSelector('#picked:not([hidden])');
    await page.check('input[value=session]');
    await page.click('#play-picked');
  } else {
    await page.goto(`${base}?nodisc&arg=--allow-software-gpu`);
  }
  // Let it get going before measuring.
  await page.waitForTimeout(8000);

  const cdp = await page.context().newCDPSession(page);
  await cdp.send('Profiler.enable');
  await cdp.send('Profiler.setSamplingInterval', { interval: 200 });
  const frames0 = await page.evaluate(() => globalThis.duskTestFrames ?? 0);
  await cdp.send('Profiler.start');
  await page.waitForTimeout(seconds * 1000);
  const { profile } = await cdp.send('Profiler.stop');
  const frames1 = await page.evaluate(() => globalThis.duskTestFrames ?? 0);
  if (savePath) await writeFile(savePath, JSON.stringify(profile));

  const byId = new Map(profile.nodes.map((n) => [n.id, n]));
  const self = new Map();
  const deltas = profile.timeDeltas;
  let total = 0;
  profile.samples.forEach((id, i) => {
    const dt = deltas[i] ?? 0;
    total += dt;
    const frame = byId.get(id).callFrame;
    const name = frame.functionName || `(anonymous ${path.basename(frame.url)}:${frame.lineNumber})`;
    self.set(name, (self.get(name) ?? 0) + dt);
  });
  const fatal = await page.evaluate(() => window.duskState?.fatal ?? null);
  console.log(`profile-page: ${((frames1 - frames0) / seconds).toFixed(1)} frames/s presented; ` +
    `${(100 * (self.get('(idle)') ?? 0) / total).toFixed(1)}% of the main thread idle`);
  if (fatal) console.log(`profile-page: the page reported a fatal error: ${fatal}`);
  for (const [name, t] of [...self].sort((a, b) => b[1] - a[1]).slice(0, top)) {
    console.log(`${(100 * t / total).toFixed(1).padStart(5)}%  ${name.slice(0, 140)}`);
  }
} finally {
  await browser.close();
  server.close();
}
